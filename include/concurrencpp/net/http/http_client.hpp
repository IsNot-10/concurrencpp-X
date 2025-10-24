#ifndef CONCURRENCPP_NET_HTTP_CLIENT_HPP
#define CONCURRENCPP_NET_HTTP_CLIENT_HPP

#include <string>
#include <iostream>
#include <chrono>
#include <sstream>
#include <system_error>
#include <stdexcept>

#include "concurrencpp/results/lazy_result.h"
#include "concurrencpp/net/asio.hpp"
#include "concurrencpp/net/asio_coro_util.hpp"
#include "concurrencpp/runtime/io_context_pool.hpp"
#include <algorithm>
#include <cctype>

namespace concurrencpp::net {

using asio::ip::tcp;

class http_client {
public:
    explicit http_client(io_context_pool& pool) : pool_(&pool) {}

    concurrencpp::lazy_result<void> http_call(std::string url) {
        auto& io_context = pool_->get_client_io_context();
        tcp::socket socket(io_context);

        // 简单 10 秒超时
        asio::steady_timer timer(io_context);
        constexpr auto timeout = std::chrono::seconds(10);
        timer.expires_after(timeout);
        timer.async_wait([&socket](const std::error_code& ec) {
            if (!ec) {
                std::error_code ignore_ec;
                socket.cancel(ignore_ec);
            }
        });

        auto u = parse_url(url);
        if (u.scheme != "http") {
            throw std::invalid_argument("only http scheme is supported");
        }

        // 连接服务器
        {
            auto ec = co_await concurrencpp::net::async_connect(io_context, socket, u.host, u.port);
            if (ec) {
                if (ec == asio::error::operation_aborted) {
                    throw asio::system_error(asio::error::timed_out);
                }
                throw asio::system_error(ec);
            }
        }

        const auto req = build_request(u);
        print_wget_request(u);
        co_await concurrencpp::net::async_write(socket, asio::buffer(req.data(), req.size()));

        asio::streambuf response;
        co_await concurrencpp::net::async_read_until(socket, response, "\r\n");
        std::istream rs(&response);
        std::string http_version;
        unsigned int status_code;
        std::string status_message;
        rs >> http_version;
        rs >> status_code;
        std::getline(rs, status_message);
        if (!rs || http_version.substr(0, 5) != "HTTP/") {
            std::cout << "Invalid response\n";
            co_return;
        }
        if (status_code != 200) {
            std::cout << "Response returned with status code " << status_code << "\n";
            co_return;
        }

        co_await concurrencpp::net::async_read_until(socket, response, "\r\n\r\n");
        std::size_t content_length = 0;
        bool has_content_length = parse_headers_and_content_length(rs, content_length);

        // 已经在缓冲里的正文先输出
        std::size_t already = response.size();
        if (already > 0) {
            std::cout << &response;
        }
        std::size_t remaining = has_content_length && content_length > already ? (content_length - already) : 0;

        if (has_content_length) {
            while (remaining > 0) {
                auto prepared = response.prepare(std::min<std::size_t>(remaining, 8192));
                auto [rec, rlen] = co_await concurrencpp::net::async_read_some(socket, prepared);
                if (rec) {
                    if (rec == asio::error::eof) {
                        response.commit(rlen);
                        if (rlen > 0) std::cout << &response;
                        break;
                    }
                    if (rec == asio::error::operation_aborted) {
                        throw asio::system_error(asio::error::timed_out);
                    }
                    throw asio::system_error(rec);
                }
                response.commit(rlen);
                if (rlen == 0) break;
                std::cout << &response;
                remaining -= rlen;
            }
        } else {
            for (;;) {
                auto prepared = response.prepare(8192);
                auto [rec, rlen] = co_await concurrencpp::net::async_read_some(socket, prepared);
                if (rec) {
                    if (rec == asio::error::eof) {
                        response.commit(rlen);
                        if (rlen > 0) std::cout << &response;
                        break;
                    }
                    if (rec == asio::error::operation_aborted) {
                        throw asio::system_error(asio::error::timed_out);
                    }
                    throw asio::system_error(rec);
                }
                response.commit(rlen);
                if (rlen == 0) break;
                std::cout << &response;
            }
        }

        // 清理资源
        std::error_code ignore_timer_ec;
        timer.cancel();
        std::error_code ignore_ec;
        socket.shutdown(tcp::socket::shutdown_both, ignore_ec);
        socket.close(ignore_ec);
        co_return;
    }

private:
    io_context_pool* pool_;

    struct parsed_url {
        std::string scheme;
        std::string host;
        std::string port;
        std::string path;
    };

    static parsed_url parse_url(const std::string& url) {
        parsed_url u{};
        u.scheme = "http";
        std::string s = url;
        auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](unsigned char ch){ return !is_space(ch); }));
        s.erase(std::find_if(s.rbegin(), s.rend(), [&](unsigned char ch){ return !is_space(ch); }).base(), s.end());

        auto pos_scheme = s.find("://");
        if (pos_scheme != std::string::npos) {
            u.scheme = s.substr(0, pos_scheme);
            s = s.substr(pos_scheme + 3);
        }
        auto pos_slash = s.find('/');
        std::string hostport = pos_slash == std::string::npos ? s : s.substr(0, pos_slash);
        u.path = pos_slash == std::string::npos ? "/" : s.substr(pos_slash);

        if (!hostport.empty() && hostport.front() == '[') {
            auto end = hostport.find(']');
            u.host = end == std::string::npos ? hostport : hostport.substr(1, end - 1);
            if (end != std::string::npos && end + 1 < hostport.size() && hostport[end + 1] == ':') {
                u.port = hostport.substr(end + 2);
            }
        } else {
            auto pos_colon = hostport.rfind(':');
            if (pos_colon != std::string::npos) {
                u.host = hostport.substr(0, pos_colon);
                u.port = hostport.substr(pos_colon + 1);
            } else {
                u.host = hostport;
            }
        }
        if (u.port.empty()) {
            u.port = (u.scheme == "https") ? "443" : "80";
        }
        if (u.host.empty()) {
            throw std::invalid_argument("invalid url: missing host");
        }
        return u;
    }

    static std::string build_request(const parsed_url& u) {
        std::string req;
        req.reserve(u.host.size() + u.port.size() + u.path.size() + 64);
        req += "GET ";
        req += u.path.empty() ? "/" : u.path;
        req += " HTTP/1.1\r\n";
        req += "Host: " + u.host + ":" + u.port + "\r\n";
        req += "Accept: */*\r\n";
        req += "User-Agent: Wget/1.14 (linux-gnu)\r\n";
        req += "Connection: close\r\n\r\n";
        return req;
    }

    static void print_wget_request(const parsed_url& u) {
        std::cout << "GET " << (u.path.empty() ? "/" : u.path) << " HTTP/1.1\n";
        std::cout << "Host: " << u.host << "\n";
        std::cout << "Accept: */*\n";
        std::cout << "User-Agent: Wget/1.14 (linux-gnu)\n";
        std::cout << "Connection: close\n\n";
    }

    static bool parse_headers_and_content_length(std::istream& rs, std::size_t& content_length) {
        content_length = 0;
        bool has_content_length = false;
        std::string header;
        while (std::getline(rs, header) && header != "\r") {
            std::cout << header << "\n";
            if (!has_content_length) {
                auto pos = header.find(':');
                if (pos != std::string::npos) {
                    std::string name = header.substr(0, pos);
                    for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (name == "content-length") {
                        std::string value = header.substr(pos + 1);
                        auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
                        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char ch){ return !is_space(ch); }));
                        value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char ch){ return !is_space(ch); }).base(), value.end());
                        try {
                            content_length = static_cast<std::size_t>(std::stoull(value));
                            has_content_length = true;
                        } catch (...) {}
                    }
                }
            }
        }
        std::cout << "\n";
        return has_content_length;
    }
};

} // namespace concurrencpp::net

#endif // CONCURRENCPP_NET_HTTP_CLIENT_HPP
