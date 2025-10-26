#include <iostream>
#include "concurrencpp/concurrencpp.h"

using namespace concurrencpp;
using namespace concurrencpp::net;

lazy_result<void> main_impl(concurrencpp::runtime* rt) {
    auto& io_pool = rt->net_io_pool();

    http_server server(io_pool, 8080, "./");

    // 简单的欢迎页面
    server.GET("/", [](const request& req, response& resp) {
        resp = build_text_response("Welcome to ConcurrenCpp HTTP Server!");
    });

    // 查询参数
    server.GET("/search", [](const request& req, response& resp) {
        std::string result = "Search Results:\n";
        
        // 获取查询参数，带默认值
        std::string query = req.query("q", "no query");
        std::string page = req.query("page", "1");
        
        result += "Query: " + query + "\n";
        result += "Page: " + page + "\n";
        
        // 检查是否有特定参数
        if (req.has_query("category")) {
            result += "Category: " + req.query("category") + "\n";
        }
        
        resp = build_text_response(result);
    });

    // 路径参数
    server.GET("/user/*", [](const request& req, response& resp) {
        std::string result = "User Profile:\n";
        
        // 获取路径参数
        std::string user_id = req.param("*");
        result += "User ID: " + user_id + "\n";
        
        // 模拟用户数据
        if (user_id == "123") {
            result += "Name: John Doe\n";
            result += "Email: john@example.com\n";
        } else {
            result += "User not found\n";
        }
        
        resp = build_text_response(result);
    });

    // POST路由，用于测试JSON解析
    server.POST("/api/user", [](const request& req, response& res) {
        if (req.is_json()) {
            auto name = req.json_value<std::string>("name", "Unknown");
            auto age = req.json_value<int>("age", 0);
            
            res.content = "Received JSON: name=" + name + ", age=" + std::to_string(age);
            res.headers.push_back({"Content-Type", "application/json"});
        } else {
            res.content = "Expected JSON content";
            res.status = status_type::bad_request;
        }
    });
    
    std::cout << "HTTP Server started on port 8080" << std::endl;
    std::cout << "Try these URLs:" << std::endl;
    std::cout << "  http://localhost:8080/" << std::endl;
    std::cout << "  http://localhost:8080/search?q=test&page=2" << std::endl;
    std::cout << "  http://localhost:8080/user/123" << std::endl;

    co_await server.start();
    
    // 保持服务器运行
    std::cout << "\nPress Enter to stop the server..." << std::endl;
    std::cin.get();
}

int main() {
    try {
        concurrencpp::runtime rt;
        auto task = main_impl(&rt);
        auto result = task.run();
        result.wait();  
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}