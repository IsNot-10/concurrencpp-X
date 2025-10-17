#ifndef CONCURRENCPP_NET_SERVER_HPP
#define CONCURRENCPP_NET_SERVER_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include <functional>
#include <system_error>
#include <iostream>

#include "concurrencpp/net/asio/asio.hpp"
#include "concurrencpp/net/io_context_pool.hpp"
#include "concurrencpp/net/asio_coro_util.hpp"
#include "concurrencpp/results/result.h"
#include "concurrencpp/results/lazy_result.h"
#include "concurrencpp/net/constants.h"

namespace concurrencpp::net {

// TCP服务器：事件循环与资源管理封装在内部；用户只需编写读写会话协程。
class CRCPP_API tcp_server {
public:
    explicit tcp_server(std::uint16_t port)
        : port_(port),
          io_threads_(constants::default_io_threads),
          running_(false) {}

    ~tcp_server() { stop(); }

    void start() {
        if (running_.exchange(true)) return;
        // 初始化IO上下文池与acceptor
        pool_ = std::make_unique<io_context_pool>(io_threads_);
        acceptor_ = std::make_unique<asio::ip::tcp::acceptor>(pool_->get_io_context());

        asio::ip::tcp::endpoint endpoint(
            asio::ip::make_address(constants::default_address), port_);
        acceptor_->open(endpoint.protocol());
        acceptor_->set_option(asio::socket_base::reuse_address(
            constants::default_reuse_address));
        std::error_code ec_bind;
        acceptor_->bind(endpoint, ec_bind);
        if (ec_bind) {
            std::cerr << "[tcp_server] bind failed: " << ec_bind.message() << std::endl;
        }
        std::error_code ec_listen;
        acceptor_->listen(constants::default_backlog, ec_listen);
        if (ec_listen) {
            std::cerr << "[tcp_server] listen failed: " << ec_listen.message() << std::endl;
        }

        // 启动服务器协程接受循环，持有其返回的 result 以避免状态泄漏
        accept_task_ = run().run();

        // 启动IO上下文线程池
        pool_->run();
    }

    void stop() {
        if (!running_.exchange(false)) return;

        if (acceptor_) {
            std::error_code ec;
            acceptor_->cancel(ec);
            acceptor_->close(ec);
        }

        // 主动关闭所有活动连接，打断挂起 I/O
        {
            std::vector<std::shared_ptr<asio::ip::tcp::socket>> local_sockets;
            {
                std::lock_guard<std::mutex> lk(sockets_mutex_);
                local_sockets.swap(sockets_);
            }
            for (auto& s : local_sockets) {
                std::error_code ec2;
                s->shutdown(asio::ip::tcp::socket::shutdown_both, ec2);
                s->close(ec2);
            }
        }

        // 等待接受循环与所有会话协程正常结束，确保释放其状态
        if (accept_task_) {
            accept_task_.wait();
        }

        {
            std::vector<concurrencpp::result<void>> local_sessions;
            {
                std::lock_guard<std::mutex> lk(sessions_mutex_);
                local_sessions.swap(sessions_);
            }
            for (auto& r : local_sessions) {
                if (r) {
                    r.wait();
                }
            }
        }

        if (pool_) {
            pool_->stop();
            pool_.reset();
        }

        acceptor_.reset();
    }

    using session_handler_t = std::function<concurrencpp::lazy_result<void>(
        std::shared_ptr<asio::ip::tcp::socket>)>;

    void set_session_handler(session_handler_t handler) {
        session_handler_ = std::move(handler);
    }

    void set_io_threads(std::size_t threads) {
        if (running_) return;
        io_threads_ = threads == 0 ? 1 : threads;
    }

private:
    concurrencpp::lazy_result<void> run() {
        for (;;) {
            auto& conn_io_context = pool_->get_io_context();
            auto socket = std::make_shared<asio::ip::tcp::socket>(conn_io_context);

            auto error = co_await concurrencpp::net::async_accept(*acceptor_, *socket);
            if (error) {
                if (error == asio::error::operation_aborted) {
                    break; // 服务器停止
                }
                continue; // 接受错误，继续
            }

            {
                std::lock_guard<std::mutex> lk(sockets_mutex_);
                sockets_.push_back(socket);
            }

            // 配置socket选项
            std::error_code ec;
            socket->set_option(asio::ip::tcp::no_delay(constants::default_tcp_no_delay), ec);
            socket->set_option(asio::socket_base::keep_alive(constants::default_keep_alive), ec);

            // 运行用户会话协程；未设置则立即关闭
            concurrencpp::lazy_result<void> session_task = session_handler_
                ? session_handler_(socket)
                : close_immediately(socket);

            // 运行会话协程并保存其 result，用于停止时等待释放资源
            {
                auto res = session_task.run();
                std::lock_guard<std::mutex> lk(sessions_mutex_);
                sessions_.push_back(std::move(res));
            }
        }
    }

    // 未提供会话处理器时，立即关闭连接
    concurrencpp::lazy_result<void> close_immediately(std::shared_ptr<asio::ip::tcp::socket> socket) {
        std::error_code ec;
        socket->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket->close(ec);
        co_return;
    }

    std::uint16_t port_;
    std::size_t io_threads_;
    std::unique_ptr<io_context_pool> pool_;
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor_;
    std::atomic<bool> running_;
    // 不跟踪会话结果，避免冗余管理逻辑
    std::mutex sockets_mutex_;
    std::vector<std::shared_ptr<asio::ip::tcp::socket>> sockets_;
    // 持有接受循环与会话协程的 result，用于优雅关闭时等待完成，避免泄漏
    concurrencpp::result<void> accept_task_;
    std::mutex sessions_mutex_;
    std::vector<concurrencpp::result<void>> sessions_;
    session_handler_t session_handler_;
};

} // namespace concurrencpp::net

#endif // CONCURRENCPP_NET_SERVER_HPP