#ifndef ASYNC_SIMPLE_DEMO_ASIO_UTIL_H
#define ASYNC_SIMPLE_DEMO_ASIO_UTIL_H
#include "concurrencpp/net/asio/asio.hpp"
template <typename AsioBuffer>
std::pair<asio::error_code, size_t> read_some(asio::ip::tcp::socket &sock,
                                              AsioBuffer &&buffer) {
    asio::error_code error;
    size_t length = sock.read_some(std::forward<AsioBuffer>(buffer), error);
    return std::make_pair(error, length);
}

template <typename AsioBuffer>
std::pair<asio::error_code, size_t> write(asio::ip::tcp::socket &sock,
                                          AsioBuffer &&buffer) {
    asio::error_code error;
    auto length = asio::write(sock, std::forward<AsioBuffer>(buffer), error);
    return std::make_pair(error, length);
}

std::pair<std::error_code, asio::ip::tcp::socket> accept(
    asio::ip::tcp::acceptor &a) {
    std::error_code error;
    auto socket = a.accept(error);
    return std::make_pair(error, std::move(socket));
}

std::pair<std::error_code, asio::ip::tcp::socket> connect(
    asio::io_context &io_context, std::string host, std::string port) {
    asio::ip::tcp::socket s(io_context);
    asio::ip::tcp::resolver resolver(io_context);
    std::error_code error;
    asio::connect(s, resolver.resolve(host, port), error);
    return std::make_pair(error, std::move(s));
}
#endif
