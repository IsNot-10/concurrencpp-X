#include <iostream>
#include <string>
#include <signal.h>
#include <atomic>
#include <thread>
#include <chrono>

#include "concurrencpp/concurrencpp.h"

using concurrencpp::lazy_result;
namespace net = concurrencpp::net;

int main(int argc, char** argv) {
    uint16_t port = concurrencpp::net::constants::default_port;
    if (argc >= 2) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }

    std::cout << "Starting echo server on port " << port << std::endl;
    std::cout << "Press Ctrl+C to exit" << std::endl;

    try {
        concurrencpp::runtime rt;

        auto& pool = rt.net_io_pool();

        net::tcp_server server(pool, port);

        auto server_result = server.start().run();
        server_result.wait();

        std::cout << "Server stopped." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}