#ifndef ASYNC_SIMPLE_CONNECTION_HPP
#define ASYNC_SIMPLE_CONNECTION_HPP

#include <fstream>
#include <string>
#include <memory>
#include <sstream>
#include <iostream>
#include <system_error>
#include "concurrencpp/net/asio_coro_util.hpp"
#include "concurrencpp/net/http/http_request.hpp"
#include "concurrencpp/net/http/http_response.hpp"
#include "concurrencpp/results/lazy_result.h"
#include <algorithm>
#include <cctype>

class connection {
public:
    connection(asio::ip::tcp::socket socket, std::string &&doc_root)
        : socket_(std::move(socket)), doc_root_(std::move(doc_root)) {}
    ~connection() {
        std::error_code ec;
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }
    concurrencpp::lazy_result<void> start() {
        for (;;) {
            auto [error, bytes_transferred] =
                co_await concurrencpp::net::async_read_some(socket_, asio::buffer(read_buf_));
            if (error) {
                if (error == asio::error::eof || error == asio::error::connection_reset || error == asio::error::operation_aborted) {

                    break;
                }
                std::cout << "error: " << error.message()
                          << ", size=" << bytes_transferred << '\n';
                break;
            }

            request_parser::result_type result;
            std::tie(result, std::ignore) = parser_.parse(
                request_, read_buf_, read_buf_ + bytes_transferred);
            if (result == request_parser::good) {
                handle_request(request_, response_);
                bool keep_alive = is_keep_alive();
                response_.headers.push_back({"Connection", keep_alive ? "keep-alive" : "close"});
                auto [write_error, write_bytes] = co_await concurrencpp::net::async_write(socket_, response_.to_buffers());
                if (write_error) {
                    if (write_error == asio::error::eof || write_error == asio::error::connection_reset || write_error == asio::error::operation_aborted) {
                        break;
                    }
                    std::cout << "Write error: " << write_error.message() << '\n';
                    break;
                }
                if (!keep_alive) {
                    break;
                }

                request_ = {};
                response_ = {};
                parser_.reset();
            } else if (result == request_parser::bad) {
                response_ = build_response(status_type::bad_request);
                response_.headers.push_back({"Connection", "close"});
                auto [write_error, write_bytes] = co_await concurrencpp::net::async_write(socket_, response_.to_buffers());
                if (write_error) {
                    if (write_error == asio::error::eof || write_error == asio::error::connection_reset || write_error == asio::error::operation_aborted) {
                        break;
                    }
                    std::cout << "Write error: " << write_error.message() << '\n';
                }
                break;
            }
        }
    }

private:
    void handle_request(const request &req, response &rep) {

        std::string request_path;
        if (!url_decode(req.uri, request_path)) {
            rep = build_response(status_type::bad_request);
            return;
        }

        if (request_path.empty() || request_path[0] != '/' ||
            request_path.find("..") != std::string::npos) {
            rep = build_response(status_type::bad_request);
            return;
        }

        if (request_path[request_path.size() - 1] == '/') {
            rep = build_response(status_type::ok);
            return;

        }

        std::size_t last_slash_pos = request_path.find_last_of("/");
        std::size_t last_dot_pos = request_path.find_last_of(".");
        std::string extension;
        if (last_dot_pos != std::string::npos &&
            last_dot_pos > last_slash_pos) {
            extension = request_path.substr(last_dot_pos + 1);
        }

        std::string full_path = doc_root_ + request_path;
        std::ifstream is(full_path.c_str(), std::ios::in | std::ios::binary);
        if (!is) {
            rep = build_response(status_type::not_found);
            return;
        }

        rep.status = status_type::ok;
        char buf[2048];
        while (is.read(buf, sizeof(buf)).gcount() > 0)
            rep.content.append(buf, is.gcount());
        rep.headers.resize(2);
        rep.headers[0].name = "Content-Length";
        rep.headers[0].value = std::to_string(rep.content.size());
        rep.headers[1].name = "Content-Type";
        rep.headers[1].value = mime_types::extension_to_type(extension);
    }

    bool url_decode(const std::string &in, std::string &out) {
        out.clear();
        out.reserve(in.size());
        for (std::size_t i = 0; i < in.size(); ++i) {
            if (in[i] == '%') {
                if (i + 3 <= in.size()) {
                    int value = 0;
                    std::istringstream is(in.substr(i + 1, 2));
                    if (is >> std::hex >> value) {
                        out += static_cast<char>(value);
                        i += 2;
                    } else {
                        return false;
                    }
                } else {
                    return false;
                }
            } else if (in[i] == '+') {
                out += ' ';
            } else {
                out += in[i];
            }
        }
        return true;
    }

    bool is_keep_alive() {
        for (const auto &h : request_.headers) {
            std::string name = h.name;
            std::string value = h.value;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c){ return std::tolower(c); });
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c){ return std::tolower(c); });
            if (name == "connection" && value == "close") {
                return false;
            }
        }
        return true;
    }

private:
    asio::ip::tcp::socket socket_;
    char read_buf_[4096];
    request_parser parser_;
    request request_;
    response response_;
    std::string doc_root_;
};

#endif
