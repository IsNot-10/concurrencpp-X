#ifndef ASYNC_SIMPLE_HTTP_REQUEST_HPP
#define ASYNC_SIMPLE_HTTP_REQUEST_HPP

#include <string>
#include <tuple>
#include <vector>

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
};

class request_parser {
public:

    request_parser() : state_(method_start) {}

    void reset() { state_ = method_start; }

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
                return (input == '\n') ? good : bad;
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
        expecting_newline_3
    } state_;
};

#endif
