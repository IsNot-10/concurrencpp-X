#include "concurrencpp/concurrencpp.h"
#include "concurrencpp/workflow/workflow.h"

#include <iostream>
#include <string>
#include <memory>
#include <chrono>
#include <thread>

using namespace concurrencpp;
using namespace concurrencpp::workflow;

// 简单打印模块：展示并行层次与执行器名称，并模拟耗时
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

    // 可选：配置工作流（并发门控、错误策略等）
    wf.set_default_priority(0);
    wf.set_priority_aging_step(1);
    wf.set_max_concurrency_per_round(4);
    wf.set_error_policy(Executor::ErrorPolicy::CancelOnError);

    // 构建 DAG：
    // 层0：A、H
    // 层1：B(A)、C(A)、I(H)
    // 层2：D(B)、E(B,C)、F(C)、J(H,E)
    // 层3：G(D,E,F)
    // 层4：K(G,I,J)
    // 层5：L(H)、M(L,A)、N(M,K)

    auto A = std::make_shared<PrintModule>("A", std::vector<std::string>{}, 40);
    auto H = std::make_shared<PrintModule>("H", std::vector<std::string>{}, 40);

    auto B = std::make_shared<PrintModule>("B", std::vector<std::string>{"A"}, 50);
    auto C = std::make_shared<PrintModule>("C", std::vector<std::string>{"A"}, 30);
    auto I = std::make_shared<PrintModule>("I", std::vector<std::string>{"H"}, 20);

    auto D = std::make_shared<PrintModule>("D", std::vector<std::string>{"B"}, 60);
    auto E = std::make_shared<PrintModule>("E", std::vector<std::string>{"B", "C"}, 40);
    auto F = std::make_shared<PrintModule>("F", std::vector<std::string>{"C"}, 25);
    auto J = std::make_shared<PrintModule>("J", std::vector<std::string>{"H", "E"}, 35);

    auto G = std::make_shared<PrintModule>("G", std::vector<std::string>{"D", "E", "F"}, 70);
    auto K = std::make_shared<PrintModule>("K", std::vector<std::string>{"G", "I", "J"}, 30);

    auto L = std::make_shared<PrintModule>("L", std::vector<std::string>{"H"}, 20);
    auto M = std::make_shared<PrintModule>("M", std::vector<std::string>{"L", "A"}, 30);
    auto N = std::make_shared<PrintModule>("N", std::vector<std::string>{"M", "K"}, 25);

    // 逐个注册模块（替代旧版的 addModules）
    for (auto& mod : {A, H, B, C, I, D, E, F, J, G, K, L, M, N}) {
        wf.addModule(mod);
    }

    // 执行工作流
    wf.execute();

    // 打印摘要与每个模块统计
    auto wstats = wf.getWorkflowStats();
    std::cout << "Workflow completed. Duration(ms): " << wstats.duration.count() << std::endl;

    std::cout << "--- Module stats ---" << std::endl;
    for (const auto& name : wf.getModuleNames()) {
        auto st = wf.getModuleState(name);
        auto stats = wf.getModuleStats(name);
        std::string st_s;
        switch (st) {
            case Executor::ModuleState::Pending: st_s = "Pending"; break;
            case Executor::ModuleState::Running: st_s = "Running"; break;
            case Executor::ModuleState::Done: st_s = "Done"; break;
            case Executor::ModuleState::Failed: st_s = "Failed"; break;
            case Executor::ModuleState::Skipped: st_s = "Skipped"; break;
        }
        std::cout << name << ": state=" << st_s << ", duration(ms)=" << stats.duration.count() << std::endl;
    }

    return 0;
}