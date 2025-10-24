#ifndef CONCURRENCPP_NET_ASIO_CORO_UTIL_H
#define CONCURRENCPP_NET_ASIO_CORO_UTIL_H
#include <sys/types.h>
#include "concurrencpp/results/lazy_result.h"

#include <chrono>
#include <concepts>
#include <cstdint>
#include <functional>
#include <coroutine>
#include "concurrencpp/net/asio.hpp"

namespace concurrencpp::net {

class CallbackAwaiter {
public:

    using CallbackFunction = std::function<void(std::coroutine_handle<>)>;

    CallbackAwaiter(CallbackFunction callback_function)
        : callback_function_(std::move(callback_function)) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle) {
        callback_function_(handle);
    }

    void await_resume() noexcept {}

private:
    CallbackFunction callback_function_;
};

template <typename T>
class CallbackAwaiterWithResult {
public:
    using CallbackFunction =
        std::function<void(std::coroutine_handle<>, std::function<void(T)>)>;

    CallbackAwaiterWithResult(CallbackFunction callback_function)
        : callback_function_(std::move(callback_function)) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle) {
        callback_function_(handle, [this](T t) { result_ = std::move(t); });
    }

    T await_resume() noexcept { return std::move(result_); }

private:
    CallbackFunction callback_function_;
    T result_;
};

class PeriodTimer : public asio::steady_timer {
public:
    using asio::steady_timer::steady_timer;

    template <typename T>
    PeriodTimer(asio::io_context &ioc) : asio::steady_timer(ioc) {}

    concurrencpp::lazy_result<bool> async_await() noexcept {
        co_return co_await CallbackAwaiterWithResult<bool>([this](auto handle, auto set_resume_value) {
            this->async_wait([&, handle, set_resume_value](const auto &ec) {
                set_resume_value(!ec);
                handle.resume();
            });
        });
    }
};

inline concurrencpp::lazy_result<std::error_code> async_accept(
    asio::ip::tcp::acceptor &acceptor, asio::ip::tcp::socket &socket) noexcept {
    co_return co_await CallbackAwaiterWithResult<std::error_code>(
        [&](std::coroutine_handle<> handle, auto set_resume_value) {
            acceptor.async_accept(
                socket, [handle, set_resume_value = std::move(
                                     set_resume_value)](auto ec) mutable {
                    set_resume_value(std::move(ec));
                    handle.resume();
                });
        });
}

template <typename Socket, typename AsioBuffer>
inline concurrencpp::lazy_result<std::pair<std::error_code, size_t>>
async_read_some(Socket &socket, AsioBuffer &&buffer) noexcept {
    co_return co_await CallbackAwaiterWithResult<std::pair<std::error_code, size_t>>(
        [&](std::coroutine_handle<> handle, auto set_resume_value) mutable {
            socket.async_read_some(
                std::move(buffer),
                [handle, set_resume_value = std::move(set_resume_value)](
                    auto ec, auto size) mutable {
                    set_resume_value(std::make_pair(std::move(ec), size));
                    handle.resume();
                });
        }
    );
}

template <typename Socket, typename AsioBuffer>
inline concurrencpp::lazy_result<std::pair<std::error_code, size_t>> async_read(
    Socket &socket, AsioBuffer &buffer) noexcept {
    co_return co_await CallbackAwaiterWithResult<std::pair<std::error_code, size_t>>(
        [&](std::coroutine_handle<> handle, auto set_resume_value) mutable {
            asio::async_read(
                socket, buffer,
                [handle, set_resume_value = std::move(set_resume_value)](
                    auto ec, auto size) mutable {
                    set_resume_value(std::make_pair(std::move(ec), size));
                    handle.resume();
                });
        }
    );
}

template <typename Socket, typename AsioBuffer>
inline concurrencpp::lazy_result<std::pair<std::error_code, size_t>>
async_read_until(Socket &socket, AsioBuffer &buffer,
                 asio::string_view delim) noexcept {
    co_return co_await CallbackAwaiterWithResult<std::pair<std::error_code, size_t>>(
        [&](std::coroutine_handle<> handle, auto set_resume_value) mutable {
            asio::async_read_until(
                socket, buffer, delim,
                [handle, set_resume_value = std::move(set_resume_value)](
                    auto ec, auto size) mutable {
                    set_resume_value(std::make_pair(std::move(ec), size));
                    handle.resume();
                });
        }
    );
}

template <typename Socket, typename AsioBuffer>
inline concurrencpp::lazy_result<std::pair<std::error_code, size_t>> async_write(
    Socket &socket, AsioBuffer &&buffer) noexcept {
    co_return co_await CallbackAwaiterWithResult<std::pair<std::error_code, size_t>>(
        [&](std::coroutine_handle<> handle, auto set_resume_value) mutable {
            asio::async_write(
                socket, std::move(buffer),
                [handle, set_resume_value = std::move(set_resume_value)](
                    auto ec, auto size) mutable {
                    set_resume_value(std::make_pair(std::move(ec), size));
                    handle.resume();
                });
        }
    );
}

inline concurrencpp::lazy_result<std::error_code> async_connect(
    asio::io_context &io_context, asio::ip::tcp::socket &socket,
    const std::string &host, const std::string &port) noexcept {
    co_return co_await CallbackAwaiterWithResult<std::error_code>(
        [&](std::coroutine_handle<> handle, auto set_resume_value) mutable {
            asio::ip::tcp::resolver resolver(io_context);
            auto endpoints = resolver.resolve(host, port);
            asio::async_connect(
                socket, endpoints,
                [handle, set_resume_value = std::move(set_resume_value)](
                    auto ec, auto ) mutable {
                    set_resume_value(std::move(ec));
                    handle.resume();
                });
        }
    );
}

}

#endif
