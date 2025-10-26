#ifndef ASYNC_SIMPLE_HTTP_REQUEST_HPP
#define ASYNC_SIMPLE_HTTP_REQUEST_HPP

#include <string>
#include <tuple>
#include <vector>
#include <unordered_map>
#include <sstream>
#include <cctype>
#include "json.hpp"

struct header {
    std::string name;
    std::string value;
};

struct request {
    std::string method;
    std::string uri;
    int http_version_major;
    int http_version_minor;
    std::vector<header> headers;
    std::string body;
    
    
    std::unordered_map<std::string, std::string> path_params;
    
private:
    mutable std::unordered_map<std::string, std::string> query_cache_;
    mutable bool query_parsed_ = false;
    
    void parse_query_params() const {
        if (query_parsed_) return;
        
        auto pos = uri.find('?');
        if (pos == std::string::npos) {
            query_parsed_ = true;
            return;
        }
        
        std::string query_string = uri.substr(pos + 1);
        std::stringstream ss(query_string);
        std::string pair;
        
        while (std::getline(ss, pair, '&')) {
            auto eq_pos = pair.find('=');
            if (eq_pos != std::string::npos) {
                std::string key = url_decode(pair.substr(0, eq_pos));
                std::string value = url_decode(pair.substr(eq_pos + 1));
                query_cache_[key] = value;
            }
        }
        query_parsed_ = true;
    }

public:
    
    static std::string url_decode(const std::string& str) {
        std::string result;
        result.reserve(str.length());
        
        for (size_t i = 0; i < str.length(); ++i) {
            if (str[i] == '%' && i + 2 < str.length()) {
                int hex_value;
                std::istringstream hex_stream(str.substr(i + 1, 2));
                if (hex_stream >> std::hex >> hex_value) {
                    result += static_cast<char>(hex_value);
                    i += 2;
                } else {
                    result += str[i];
                }
            } else if (str[i] == '+') {
                result += ' ';
            } else {
                result += str[i];
            }
        }
        return result;
    }

    // 查询参数相关方法
    std::string query(const std::string& key, const std::string& default_value = "") const {
        parse_query_params();
        auto it = query_cache_.find(key);
        return it != query_cache_.end() ? it->second : default_value;
    }

    std::string default_query(const std::string& key, const std::string& default_value) const {
        return query(key, default_value);
    }

    bool has_query(const std::string& key = "") const {
        parse_query_params();
        if (key.empty()) {
            return !query_cache_.empty();
        }
        return query_cache_.find(key) != query_cache_.end();
    }

    std::unordered_map<std::string, std::string> query_list() const {
        parse_query_params();
        return query_cache_;
    }

    
    std::string param(const std::string& key, const std::string& default_value = "") const {
        auto it = path_params.find(key);
        return it != path_params.end() ? it->second : default_value;
    }

    bool has_param(const std::string& key) const {
        return path_params.find(key) != path_params.end();
    }

    
    std::string header_value(const std::string& name) const {
        for (const auto& h : headers) {
            if (h.name == name) {
                return h.value;
            }
        }
        return "";
    }

    bool has_header(const std::string& name) const {
        for (const auto& h : headers) {
            if (h.name == name) {
                return true;
            }
        }
        return false;
    }

    
    nlohmann::json json() const {
        try {
            return nlohmann::json::parse(body);
        } catch (const nlohmann::json::parse_error&) {
            return nlohmann::json{};
        }
    }

    bool is_json() const {
        auto content_type = header_value("Content-Type");
        return content_type.find("application/json") != std::string::npos;
    }

    template<typename T>
    T json_value(const std::string& key, const T& default_value = T{}) const {
        try {
            auto j = json();
            if (j.contains(key)) {
                return j[key].get<T>();
            }
        } catch (const nlohmann::json::exception&) {
            
        }
        return default_value;
    }
};

class request_parser {
public:

    request_parser() : state_(method_start), content_length_(0), body_bytes_read_(0) {}

    void reset() { 
        state_ = method_start; 
        content_length_ = 0;
        body_bytes_read_ = 0;
    }

    enum result_type { good, bad, indeterminate };

    template <typename InputIterator>
    std::tuple<result_type, InputIterator> parse(request& req,
                                                 InputIterator begin,
                                                 InputIterator end) {
        while (begin != end) {
            result_type result = consume(req, *begin++);
            if (result == good || result == bad)
                return std::make_tuple(result, begin);
        }
        return std::make_tuple(indeterminate, begin);
    }

private:

    result_type consume(request& req, char input) {
        switch (state_) {
            case method_start:
                if (!is_char(input) || is_ctl(input) || is_tspecial(input)) {
                    return bad;
                } else {
                    state_ = method;
                    req.method.push_back(input);
                    return indeterminate;
                }
            case method:
                if (input == ' ') {
                    state_ = uri;
                    return indeterminate;
                } else if (!is_char(input) || is_ctl(input) ||
                           is_tspecial(input)) {
                    return bad;
                } else {
                    req.method.push_back(input);
                    return indeterminate;
                }
            case uri:
                if (input == ' ') {
                    state_ = http_version_h;
                    return indeterminate;
                } else if (is_ctl(input)) {
                    return bad;
                } else {
                    req.uri.push_back(input);
                    return indeterminate;
                }
            case http_version_h:
                if (input == 'H') {
                    state_ = http_version_t_1;
                    return indeterminate;
                } else {
                    return bad;
                }
            case http_version_t_1:
                if (input == 'T') {
                    state_ = http_version_t_2;
                    return indeterminate;
                } else {
                    return bad;
                }
            case http_version_t_2:
                if (input == 'T') {
                    state_ = http_version_p;
                    return indeterminate;
                } else {
                    return bad;
                }
            case http_version_p:
                if (input == 'P') {
                    state_ = http_version_slash;
                    return indeterminate;
                } else {
                    return bad;
                }
            case http_version_slash:
                if (input == '/') {
                    req.http_version_major = 0;
                    req.http_version_minor = 0;
                    state_ = http_version_major_start;
                    return indeterminate;
                } else {
                    return bad;
                }
            case http_version_major_start:
                if (is_digit(input)) {
                    req.http_version_major =
                        req.http_version_major * 10 + input - '0';
                    state_ = http_version_major;
                    return indeterminate;
                } else {
                    return bad;
                }
            case http_version_major:
                if (input == '.') {
                    state_ = http_version_minor_start;
                    return indeterminate;
                } else if (is_digit(input)) {
                    req.http_version_major =
                        req.http_version_major * 10 + input - '0';
                    return indeterminate;
                } else {
                    return bad;
                }
            case http_version_minor_start:
                if (is_digit(input)) {
                    req.http_version_minor =
                        req.http_version_minor * 10 + input - '0';
                    state_ = http_version_minor;
                    return indeterminate;
                } else {
                    return bad;
                }
            case http_version_minor:
                if (input == '\r') {
                    state_ = expecting_newline_1;
                    return indeterminate;
                } else if (is_digit(input)) {
                    req.http_version_minor =
                        req.http_version_minor * 10 + input - '0';
                    return indeterminate;
                } else {
                    return bad;
                }
            case expecting_newline_1:
                if (input == '\n') {
                    state_ = header_line_start;
                    return indeterminate;
                } else {
                    return bad;
                }
            case header_line_start:
                if (input == '\r') {
                    state_ = expecting_newline_3;
                    return indeterminate;
                } else if (!req.headers.empty() &&
                           (input == ' ' || input == '\t')) {
                    state_ = header_lws;
                    return indeterminate;
                } else if (!is_char(input) || is_ctl(input) ||
                           is_tspecial(input)) {
                    return bad;
                } else {
                    req.headers.push_back(header());
                    req.headers.back().name.push_back(input);
                    state_ = header_name;
                    return indeterminate;
                }
            case header_lws:
                if (input == '\r') {
                    state_ = expecting_newline_2;
                    return indeterminate;
                } else if (input == ' ' || input == '\t') {
                    return indeterminate;
                } else if (is_ctl(input)) {
                    return bad;
                } else {
                    state_ = header_value;
                    req.headers.back().value.push_back(input);
                    return indeterminate;
                }
            case header_name:
                if (input == ':') {
                    state_ = space_before_header_value;
                    return indeterminate;
                } else if (!is_char(input) || is_ctl(input) ||
                           is_tspecial(input)) {
                    return bad;
                } else {
                    req.headers.back().name.push_back(input);
                    return indeterminate;
                }
            case space_before_header_value:
                if (input == ' ') {
                    state_ = header_value;
                    return indeterminate;
                } else {
                    return bad;
                }
            case header_value:
                if (input == '\r') {
                    state_ = expecting_newline_2;
                    return indeterminate;
                } else if (is_ctl(input)) {
                    return bad;
                } else {
                    req.headers.back().value.push_back(input);
                    return indeterminate;
                }
            case expecting_newline_2:
                if (input == '\n') {
                    state_ = header_line_start;
                    return indeterminate;
                } else {
                    return bad;
                }
            case expecting_newline_3:
                if (input == '\n') {
                    // 检查是否有Content-Length头
                    for (const auto& h : req.headers) {
                        if (h.name == "Content-Length") {
                            content_length_ = std::stoul(h.value);
                            break;
                        }
                    }
                    
                    if (content_length_ > 0) {
                        state_ = body_reading;
                        body_bytes_read_ = 0;
                        req.body.reserve(content_length_);
                        return indeterminate;
                    } else {
                        return good;
                    }
                } else {
                    return bad;
                }
            case body_reading:
                req.body.push_back(input);
                body_bytes_read_++;
                if (body_bytes_read_ >= content_length_) {
                    return good;
                }
                return indeterminate;
            default:
                return bad;
        }
    }

    static bool is_char(int c) { return c >= 0 && c <= 127; }

    static bool is_ctl(int c) { return (c >= 0 && c <= 31) || (c == 127); }

    static bool is_tspecial(int c) {
        switch (c) {
            case '(':
            case ')':
            case '<':
            case '>':
            case '@':
            case ',':
            case ';':
            case ':':
            case '\\':
            case '"':
            case '/':
            case '[':
            case ']':
            case '?':
            case '=':
            case '{':
            case '}':
            case ' ':
            case '\t':
                return true;
            default:
                return false;
        }
    }

    static bool is_digit(int c) { return c >= '0' && c <= '9'; }

    enum state {
        method_start,
        method,
        uri,
        http_version_h,
        http_version_t_1,
        http_version_t_2,
        http_version_p,
        http_version_slash,
        http_version_major_start,
        http_version_major,
        http_version_minor_start,
        http_version_minor,
        expecting_newline_1,
        header_line_start,
        header_lws,
        header_name,
        space_before_header_value,
        header_value,
        expecting_newline_2,
        expecting_newline_3,
        body_reading
    } state_;
    
    size_t content_length_ = 0;
    size_t body_bytes_read_ = 0;
};

#endif
