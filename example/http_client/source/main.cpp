#include <iostream>
#include <string>
#include <thread>

#include "concurrencpp/concurrencpp.h"
#include "concurrencpp/net/client.hpp"

using concurrencpp::lazy_result;
namespace net = concurrencpp::net;

lazy_result<void> http_get(net::tcp_client& client, const std::string& host, const std::string& port) {
    std::string request;
    request.reserve(128);
    request += "GET / HTTP/1.1\r\n";
    request += "Host: " + host + ":" + port + "\r\n";
    request += "User-Agent: concurrencpp-http-client/1.0\r\n";
    request += "Accept: */*\r\n";
    request += "Connection: close\r\n\r\n";

    auto response = co_await client.call(host, port, request);
    std::cout << response;
}

int main(int argc, char** argv) {
    std::string url = "http://example.com:80/";
    if (argc >= 2) url = argv[1];

    try {
        concurrencpp::runtime rt;
        auto& pool = rt.net_io_pool();

        //net::tcp_client client(pool);

        net::http_client http_client(pool);
        auto result = http_client.http_call(url);
        result.run().wait();

        //std::cout << "HTTP GET " << host << ":" << port << "/" << std::endl;
        //http_get(client, host, port).run().wait();

        //std::cout << "Done." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "http_client error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}