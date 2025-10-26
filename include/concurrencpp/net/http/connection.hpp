#ifndef ASYNC_SIMPLE_CONNECTION_HPP
#define ASYNC_SIMPLE_CONNECTION_HPP

#include <fstream>
#include <string>
#include <memory>
#include <sstream>
#include <iostream>
#include <system_error>
#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_map>

#include "concurrencpp/net/asio_coro_util.hpp"
#include "concurrencpp/net/http/http_request.hpp"
#include "concurrencpp/net/http/http_response.hpp"
#include "concurrencpp/results/lazy_result.h"

namespace concurrencpp::net {

using RouteHandler = std::function<void(const request&, response&)>;

class connection {
public:
    connection(asio::ip::tcp::socket socket, std::string &&doc_root, 
               std::unordered_map<std::string, RouteHandler> routes = {})
        : socket_(std::move(socket)), doc_root_(std::move(doc_root)), routes_(std::move(routes)) {}
    
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
                if (is_connection_closed_error(error)) {
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
                
                ensure_response_headers(keep_alive);
                
                auto [write_error, write_bytes] = co_await concurrencpp::net::async_write(socket_, response_.to_buffers());
                if (write_error) {
                    if (is_connection_closed_error(write_error)) {
                        break;
                    }
                    std::cout << "Write error: " << write_error.message() << '\n';
                    break;
                }
                
                if (!keep_alive) {
                    break;
                }

                reset_for_next_request();
            } else if (result == request_parser::bad) {
                response_ = build_response(status_type::bad_request);
                response_.headers.push_back({"Connection", "close"});
                auto [write_error, write_bytes] = co_await concurrencpp::net::async_write(socket_, response_.to_buffers());
                break;
            }
        }
    }

private:
    void handle_request(const request &req, response &rep) {
        
        std::string request_path = request::url_decode(req.uri);
        
        if (request_path.empty() || request_path[0] != '/') {
            rep = create_error_response(status_type::bad_request);
            return;
        }

        
        std::string path_only = extract_path_from_uri(request_path);
        std::string route_key = req.method + " " + path_only;
        
        std::cout << "Request: " << req.method << " " << request_path << std::endl;
        std::cout << "Route key: " << route_key << std::endl;
        
        // 查找精确匹配的路由
        if (try_exact_route_match(route_key, req, rep)) {
            return;
        }
        
        
        if (try_wildcard_route_match(route_key, req, rep)) {
            return;
        }

        std::cout << "No route found for: " << route_key << ", trying static file" << std::endl;
        serve_static_file(request_path, rep);
        
        
        if (rep.status == status_type::not_found) {
            try_default_route(req, rep);
        }
    }
    
    bool try_exact_route_match(const std::string& route_key, const request& req, response& rep) {
        auto it = routes_.find(route_key);
        if (it != routes_.end()) {
            std::cout << "Found exact match for: " << route_key << std::endl;
            it->second(req, rep);
            return true;
        }
        return false;
    }
    
    bool try_wildcard_route_match(const std::string& route_key, const request& req, response& rep) {
        for (const auto& [pattern, handler] : routes_) {
            if (match_route(pattern, route_key)) {
                std::cout << "Found wildcard match: " << pattern << " for " << route_key << std::endl;
                
                request modified_req = req;
                extract_path_params(pattern, route_key, modified_req);
                
                handler(modified_req, rep);
                return true;
            }
        }
        return false;
    }
    
    void try_default_route(const request& req, response& rep) {
        auto default_it = routes_.find("* *");
        if (default_it != routes_.end()) {
            std::cout << "Using default route" << std::endl;
            default_it->second(req, rep);
        }
    }
    
    void extract_path_params(const std::string& pattern, const std::string& route, request& req) {
        size_t star_pos = pattern.find('*');
        if (star_pos == std::string::npos) {
            return;
        }
        
        std::string prefix = pattern.substr(0, star_pos);
        if (route.size() > prefix.size()) {
            std::string param_value = route.substr(prefix.size());
            req.path_params["*"] = param_value;
        }
    }

    bool match_route(const std::string& pattern, const std::string& route) {
        if (pattern == "* *") {
            return false; 
        }
        
        if (pattern.find('*') == std::string::npos) {
            return pattern == route;
        }
        
        size_t star_pos = pattern.find('*');
        std::string prefix = pattern.substr(0, star_pos);
        
        return route.size() >= prefix.size() && 
               route.substr(0, prefix.size()) == prefix;
    }
    
    void serve_static_file(const std::string& request_path, response& rep) {
        if (request_path.find("..") != std::string::npos) {
            rep = create_error_response(status_type::bad_request);
            return;
        }

        if (request_path.back() == '/') {
            rep = build_response(status_type::ok);
            return;
        }

        std::string extension = extract_file_extension(request_path);
        std::string full_path = build_full_path(request_path);
        
        std::cout << "Trying to serve static file: " << full_path << std::endl;
        std::ifstream is(full_path.c_str(), std::ios::in | std::ios::binary);
        if (!is) {
            std::cout << "Static file not found: " << full_path << std::endl;
            rep = create_error_response(status_type::not_found);
            return;
        }

        std::cout << "Successfully serving static file: " << full_path << std::endl;
        
        
        std::string content;
        char buf[2048];
        while (is.read(buf, sizeof(buf)).gcount() > 0) {
            content.append(buf, is.gcount());
        }
        
        
        std::string content_type = std::string(mime_types::extension_to_type(extension));
        rep = response_builder::create(status_type::ok, content, content_type);
    }

    
    std::string extract_path_from_uri(const std::string& uri) {
        size_t query_pos = uri.find('?');
        return (query_pos != std::string::npos) ? uri.substr(0, query_pos) : uri;
    }
    
    std::string extract_file_extension(const std::string& path) {
        std::size_t last_slash_pos = path.find_last_of("/");
        std::size_t last_dot_pos = path.find_last_of(".");
        
        if (last_dot_pos != std::string::npos && last_dot_pos > last_slash_pos) {
            return path.substr(last_dot_pos + 1);
        }
        return "";
    }
    
    std::string build_full_path(const std::string& request_path) {
        std::string full_path = doc_root_;
        if (!doc_root_.empty() && doc_root_.back() != '/' && request_path.front() != '/') {
            full_path += "/";
        }
        if (request_path.front() == '/' && !doc_root_.empty() && doc_root_.back() == '/') {
            full_path += request_path.substr(1);
        } else if (request_path.front() == '/') {
            full_path += request_path.substr(1);
        } else {
            full_path += request_path;
        }
        return full_path;
    }

    // 使用request中的URL解码函数
    // 移除重复的url_decode_path函数，使用http_request.hpp中的静态版本

    bool is_keep_alive() const {
        for (const auto& h : request_.headers) {
            if (h.name == "Connection") {
                return h.value == "keep-alive";
            }
        }
        return false;
    }
    
    void ensure_response_headers(bool keep_alive) {
        bool has_content_length = false;
        bool has_content_type = false;
        bool has_connection = false;
        
        for (const auto& header : response_.headers) {
            if (header.name == "Content-Length") has_content_length = true;
            else if (header.name == "Content-Type") has_content_type = true;
            else if (header.name == "Connection") has_connection = true;
        }
        
        if (!has_content_length) {
            response_.headers.push_back({"Content-Length", std::to_string(response_.content.size())});
        }
        if (!has_content_type) {
            response_.headers.push_back({"Content-Type", "text/plain"});
        }
        if (!has_connection) {
            response_.headers.push_back({"Connection", keep_alive ? "keep-alive" : "close"});
        }
    }
    
    void reset_for_next_request() {
        request_ = {};
        response_ = {};
        parser_.reset();
    }
    
    bool is_connection_closed_error(const std::error_code& error) {
        return error == asio::error::eof || 
               error == asio::error::connection_reset || 
               error == asio::error::operation_aborted;
    }

private:
    asio::ip::tcp::socket socket_;
    char read_buf_[4096];
    request_parser parser_;
    request request_;
    response response_;
    std::string doc_root_;
    std::unordered_map<std::string, RouteHandler> routes_;
};

} // namespace concurrencpp::net

#endif
