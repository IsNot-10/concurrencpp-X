#include <iostream>
#include <thread>
#include "concurrencpp/concurrencpp.h"

using namespace concurrencpp::net;

int main(int argc, char** argv) {
    unsigned short port = 8080;

    if (argc >= 2) port = static_cast<unsigned short>(std::stoi(argv[1]));

    try {
        concurrencpp::runtime rt;

        auto& pool = rt.net_io_pool();
        http_server server(pool, port);
        std::cout << "HTTP server starting on port " << port << std::endl;

        auto task = server.start();
        auto result = task.run();

        std::cout << "Server running. Press Ctrl+C to stop." << std::endl;
        result.wait();

        std::cout << "Server stopped." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "http_server error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}