#ifndef CONCURRENCPP_NET_ASIO_CORO_UTIL_H
#define CONCURRENCPP_NET_ASIO_CORO_UTIL_H
#include <sys/types.h>
#include "concurrencpp/concurrencpp.h"


#include <chrono>
#include <concepts>
#include <cstdint>
#include <functional>
#include <coroutine>
#include "concurrencpp/net/asio.hpp"

namespace concurrencpp::net {

// 基础回调等待器模板
// 用于将异步回调转换为协程等待

// 基于回调的等待器基类
// 负责将异步回调机制转换为协程等待模式

// 基于回调的等待器实现
class CallbackAwaiter {
public:
    // 回调函数类型定义
    using CallbackFunction = std::function<void(std::coroutine_handle<>)>;
    
    CallbackAwaiter(CallbackFunction callback_function)
        : callback_function_(std::move(callback_function)) {}

    // 协程等待器接口方法
    bool await_ready() const noexcept { return false; }
    
    void await_suspend(std::coroutine_handle<> handle) {
        callback_function_(handle);
    }
    
    void await_resume() noexcept {}

private:
    CallbackFunction callback_function_;
};

// 基于回调的等待器，支持返回值
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

// 定期定时器包装器
class PeriodTimer : public asio::steady_timer {
public:
    using asio::steady_timer::steady_timer;
    
    template <typename T>
    PeriodTimer(asio::io_context &ioc) : asio::steady_timer(ioc) {}

    // 异步等待定时器触发
    concurrencpp::lazy_result<bool> async_await() noexcept {
        co_return co_await CallbackAwaiterWithResult<bool>([this](auto handle, auto set_resume_value) {
            this->async_wait([&, handle, set_resume_value](const auto &ec) {
                set_resume_value(!ec);
                handle.resume();
            });
        });
    }
};



// Asio 回调等待器 - 用于将 asio 的回调风格 API 转换为协程
// 支持返回特定类型的结果

// 异步接受连接
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

// 异步读取部分数据
// 支持多种 socket 类型和缓冲区类型

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

// 异步读取完整数据
// 直到缓冲区填满或发生错误

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

// 异步读取直到遇到分隔符

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

// 异步写入数据

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

// 异步连接到指定主机和端口
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
                    auto ec, auto /*endpoint*/) mutable {
                    set_resume_value(std::move(ec));
                    handle.resume();
                });
        }
    );
}

} // namespace concurrencpp::net

#endif