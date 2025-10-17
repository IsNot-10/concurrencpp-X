#include "concurrencpp/concurrencpp.h"
#include "concurrencpp/workflow/workflow.h"

#include <iostream>
#include <string>
#include <memory>
#include <chrono>
#include <thread>

using namespace concurrencpp;
using namespace concurrencpp::workflow;

// 更复杂 DAG 的打印模块：展示并行层次与执行器名称
class PrintModule : public Module {
    int work_ms_;
public:
    explicit PrintModule(std::string name, std::vector<std::string> depends = {}, int work_ms = 30)
        : Module(std::move(name), std::move(depends)), work_ms_(work_ms) {}

    result<void> execute_async(std::shared_ptr<executor> ex) override {
        co_await resume_on(ex);
        std::cout << "[" << getName() << "] start on '" << ex->name
                  << "' (tid=" << std::this_thread::get_id() << ")" << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(work_ms_));
        std::cout << "[" << getName() << "] done" << std::endl;
        co_return;
    }
};

int main() {
    Executor wf;

    // DAG：
    // 层0：A、H
    // 层1：B(A)、C(A)、I(H)
    // 层2：D(B)、E(B,C)、F(C)、J(H,E)
    // 层3：G(D,E,F)
    // 层4：K(G,I,J)
    // 层5：L(H)、M(L,A)、N(M,K)

    auto A = std::make_shared<PrintModule>("A");
    auto H = std::make_shared<PrintModule>("H");

    auto B = std::make_shared<PrintModule>("B", std::vector<std::string>{"A"});
    auto C = std::make_shared<PrintModule>("C", std::vector<std::string>{"A"});
    auto I = std::make_shared<PrintModule>("I", std::vector<std::string>{"H"});

    auto D = std::make_shared<PrintModule>("D", std::vector<std::string>{"B"});
    auto E = std::make_shared<PrintModule>("E", std::vector<std::string>{"B", "C"});
    auto F = std::make_shared<PrintModule>("F", std::vector<std::string>{"C"});
    auto J = std::make_shared<PrintModule>("J", std::vector<std::string>{"H", "E"});

    auto G = std::make_shared<PrintModule>("G", std::vector<std::string>{"D", "E", "F"});
    auto K = std::make_shared<PrintModule>("K", std::vector<std::string>{"G", "I", "J"});

    auto L = std::make_shared<PrintModule>("L", std::vector<std::string>{"H"});
    auto M = std::make_shared<PrintModule>("M", std::vector<std::string>{"L", "A"});
    auto N = std::make_shared<PrintModule>("N", std::vector<std::string>{"M", "K"});

    wf.addModules({A, H, B, C, I, D, E, F, J, G, K, L, M, N});

    wf.execute();

    std::cout << "Workflow completed. Done." << std::endl;
    return 0;
}