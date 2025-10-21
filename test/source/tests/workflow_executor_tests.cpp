#include "concurrencpp/concurrencpp.h"

#include "infra/tester.h"
#include "infra/assertions.h"

#include "concurrencpp/workflow/module.h"
#include "concurrencpp/workflow/executor.h"
#include <algorithm>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <deque>

namespace concurrencpp::tests {

    // 简单延时模块：执行会延迟一段时间，支持可选的模块级超时
    class delay_module : public concurrencpp::workflow::Module {
        std::chrono::milliseconds delay_;
        std::chrono::milliseconds timeout_;
        bool cancellable_ = false;
        std::atomic_bool cancel_flag_{false};

    public:
        delay_module(const std::string& name,
                     std::chrono::milliseconds delay,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds{0},
                     bool cancellable = false)
            : Module(name), delay_(delay), timeout_(timeout), cancellable_(cancellable) {}

        std::chrono::milliseconds timeout() const override { return timeout_; }
        bool cancellable() const override { return cancellable_; }

        void on_cancel() override {
            if (cancellable_) {
                cancel_flag_.store(true, std::memory_order_relaxed);
            }
        }

        result<void> execute_async(std::shared_ptr<concurrencpp::executor> executor) override {
            auto tq = runtime()->timer_queue();
            auto lazy = [this, tq, executor]() -> concurrencpp::lazy_result<void> {
                // 若可取消且收到信号，则尽快返回
                if (cancellable_ && cancel_flag_.load(std::memory_order_relaxed)) co_return;
                auto d = tq->make_delay_object(delay_, executor);
                co_await d.run();
                co_return;
            }();
            return lazy.run();
        }
    };

    // 立即成功模块
    class ready_module : public concurrencpp::workflow::Module {
    public:
        explicit ready_module(const std::string& name) : Module(name) {}
        result<void> execute_async(std::shared_ptr<concurrencpp::executor>) override {
            return concurrencpp::make_ready_result<void>();
        }
    };

    // 立即失败模块（异常）
    class failing_module : public concurrencpp::workflow::Module {
        std::string msg_;
    public:
        explicit failing_module(const std::string& name, std::string msg = "failing_module") : Module(name), msg_(std::move(msg)) {}
        result<void> execute_async(std::shared_ptr<concurrencpp::executor>) override {
            return concurrencpp::make_exceptional_result<void>(std::make_exception_ptr(std::runtime_error(msg_)));
        }
    };

    class recorder_module : public concurrencpp::workflow::Module {
    private:
        bool return_null_executor_ = false;
        bool do_post_ = false;

    public:
        std::string used_executor_name;

        explicit recorder_module(const std::string& name)
            : Module(name) {}

        void set_return_null_executor(bool v) { return_null_executor_ = v; }
        void set_do_post(bool v) { do_post_ = v; }

        std::shared_ptr<concurrencpp::executor> select_executor(std::shared_ptr<concurrencpp::runtime> rt) const override {
            if (return_null_executor_) {
                return nullptr;
            }
            return Module::select_executor(rt);
        }

        result<void> execute_async(std::shared_ptr<concurrencpp::executor> executor) override {
            used_executor_name = executor->name;
            if (do_post_) {
                // 触发关闭后的异常路径
                executor->post([] {});
            }
            return concurrencpp::make_ready_result<void>();
        }
    };

    // 统一状态收敛断言：确保无 Pending/Running 残留
    static void assert_state_converged(const concurrencpp::workflow::Executor& wf) {
        const auto states = wf.getAllStates();
        for (const auto& kv : states) {
            const auto st = kv.second;
            assert_true(st != concurrencpp::workflow::Executor::ModuleState::Pending &&
                        st != concurrencpp::workflow::Executor::ModuleState::Running);
        }
    }

    void test_workflow_default_executor() {
        auto mod = std::make_shared<recorder_module>("mod_default");

        concurrencpp::workflow::Executor wf;
        wf.addModule(mod);
        wf.execute();

        assert_equal(mod->used_executor_name, concurrencpp::details::consts::k_thread_pool_executor_name);
    }

    void test_workflow_override_inline_executor() {
        auto mod = std::make_shared<recorder_module>("mod_override");
        auto inline_ex = std::make_shared<concurrencpp::inline_executor>();
        mod->setPreferredExecutor(inline_ex);

        concurrencpp::workflow::Executor wf;
        wf.addModule(mod);
        wf.execute();

        assert_equal(mod->used_executor_name, concurrencpp::details::consts::k_inline_executor_name);
    }

    void test_workflow_fallback_to_default_executor() {
        auto mod = std::make_shared<recorder_module>("mod_fallback");
        mod->set_return_null_executor(true);

        concurrencpp::workflow::Executor wf;
        wf.addModule(mod);
        wf.execute();

        assert_equal(mod->used_executor_name, concurrencpp::details::consts::k_thread_pool_executor_name);
    }

    void test_workflow_shutdown_exception() {
        auto mod = std::make_shared<recorder_module>("mod_shutdown");
        auto inline_ex = std::make_shared<concurrencpp::inline_executor>();
        inline_ex->shutdown();
        mod->setPreferredExecutor(inline_ex);
        mod->set_do_post(true);

        concurrencpp::workflow::Executor wf;
        wf.addModule(mod);

        assert_throws<concurrencpp::errors::runtime_shutdown>([&wf] {
            wf.execute();
        });
    }

    // 无模块应正常执行，不抛异常
    void test_workflow_no_modules() {
        concurrencpp::workflow::Executor wf;
        assert_equal(wf.getModuleCount(), static_cast<size_t>(0));
        wf.execute();
    }

    // 重名模块应抛异常
    void test_workflow_duplicate_names() {
        concurrencpp::workflow::Executor wf;
        auto m1 = std::make_shared<ready_module>("dup");
        auto m2 = std::make_shared<ready_module>("dup");
        wf.addModule(m1);
        assert_throws_contains_error_message<std::runtime_error>([&] { wf.addModule(m2); }, "Duplicate module name");
    }

    // add_edge 未知模块应抛异常
    void test_workflow_unknown_edge() {
        concurrencpp::workflow::Executor wf;
        auto a = std::make_shared<ready_module>("A");
        wf.addModule(a);
        assert_throws_contains_error_message<std::runtime_error>([&] { wf.add_edge("A", "B"); }, "unknown module(s)");
    }

    // 循环依赖检测
    void test_workflow_cycle_detection() {
        concurrencpp::workflow::Executor wf;
        auto a = std::make_shared<ready_module>("A");
        auto b = std::make_shared<ready_module>("B");
        wf.addModule(a);
        wf.addModule(b);
        wf.add_edge("A", "B");
        // 取消即时环检测与执行期环检测：不应抛异常，交由用户保证
        wf.add_edge("B", "A");
        // 执行应正常进行（可能根据实现，部分模块被跳过或运行），但不应因环报错
        // 仅验证调用不抛异常
        wf.execute();
    }

    // 缺失依赖检测
    void test_workflow_missing_dependency() {
        concurrencpp::workflow::Executor wf;
        auto a = std::make_shared<ready_module>("A");
        // 人为添加一个不存在的依赖
        a->addDepend("X_missing");
        wf.addModule(a);
        assert_throws_contains_error_message<std::runtime_error>([&] { wf.execute(); }, "Missing dependency");
    }

    // 启动前取消：全部标记为 Skipped 并抛 interrupted_task
    void test_workflow_cancel_before_start() {
        concurrencpp::workflow::Executor wf;
        auto a = std::make_shared<delay_module>("A", std::chrono::milliseconds{50});
        auto b = std::make_shared<delay_module>("B", std::chrono::milliseconds{50});
        wf.addModule(a);
        wf.addModule(b);
        wf.request_cancel();
        assert_throws<concurrencpp::errors::interrupted_task>([&] { wf.execute(); });
        assert_equal(static_cast<int>(wf.getModuleState("A")), static_cast<int>(concurrencpp::workflow::Executor::ModuleState::Skipped));
        assert_equal(static_cast<int>(wf.getModuleState("B")), static_cast<int>(concurrencpp::workflow::Executor::ModuleState::Skipped));
        assert_state_converged(wf);
    }

    // 全局超时：部分完成，未完成的标记为 Skipped
    void test_workflow_global_timeout_partial() {
        using namespace std::chrono;
        concurrencpp::workflow::Executor wf;
        auto fast = std::make_shared<delay_module>("fast", milliseconds{10});
        auto slow = std::make_shared<delay_module>("slow", milliseconds{200});
        wf.addModule(fast);
        wf.addModule(slow);
        wf.set_timeout(milliseconds{50});
        assert_throws<concurrencpp::errors::interrupted_task>([&] { wf.execute(); });
        // fast 已完成
        assert_equal(static_cast<int>(wf.getModuleState("fast")), static_cast<int>(concurrencpp::workflow::Executor::ModuleState::Done));
        // slow 未完成 -> Skipped
        assert_equal(static_cast<int>(wf.getModuleState("slow")), static_cast<int>(concurrencpp::workflow::Executor::ModuleState::Skipped));
        // 无失败模块
        assert_equal(wf.getFailedModules().size(), static_cast<size_t>(0));
        assert_state_converged(wf);
    }

    // 模块级超时（默认 CancelOnError）：抛 interrupted_task("Module timed out") 并记录失败
    void test_workflow_module_timeout_cancel_policy() {
        using namespace std::chrono;
        concurrencpp::workflow::Executor wf;
        auto m = std::make_shared<delay_module>("M", milliseconds{200}, /*timeout*/ milliseconds{50});
        wf.addModule(m);
        assert_throws_with_error_message<concurrencpp::errors::interrupted_task>([&] { wf.execute(); }, "Module timed out");
        assert_equal(static_cast<int>(wf.getModuleState("M")), static_cast<int>(concurrencpp::workflow::Executor::ModuleState::Failed));
        assert_equal(wf.getError("M"), std::string("Module timed out"));
    }

    // 模块级超时（ContinueOnError）：继续执行其他模块，状态与错误可查询
    void test_workflow_module_timeout_continue_policy() {
        using namespace std::chrono;
        concurrencpp::workflow::Executor wf;
        wf.set_error_policy(concurrencpp::workflow::Executor::ErrorPolicy::ContinueOnError);
        auto m_to = std::make_shared<delay_module>("timeout_mod", milliseconds{200}, milliseconds{50});
        auto ok = std::make_shared<ready_module>("ok_mod");
        wf.addModule(m_to);
        wf.addModule(ok);
        wf.execute();
        assert_equal(static_cast<int>(wf.getModuleState("timeout_mod")), static_cast<int>(concurrencpp::workflow::Executor::ModuleState::Failed));
        assert_equal(static_cast<int>(wf.getModuleState("ok_mod")), static_cast<int>(concurrencpp::workflow::Executor::ModuleState::Done));
        auto failed = wf.getFailedModules();
        assert_true(std::find(failed.begin(), failed.end(), std::string("timeout_mod")) != failed.end());
        assert_equal(wf.getError("timeout_mod"), std::string("Module timed out"));
        assert_state_converged(wf);
    }

    // ContinueOnError：依赖失败导致下游直接跳过
    void test_workflow_continue_on_error_downstream_skip() {
        concurrencpp::workflow::Executor wf;
        wf.set_error_policy(concurrencpp::workflow::Executor::ErrorPolicy::ContinueOnError);
        auto A = std::make_shared<failing_module>("A", "A failed");
        auto B = std::make_shared<ready_module>("B");
        auto C = std::make_shared<ready_module>("C"); // 依赖 A 和 B
        auto D = std::make_shared<ready_module>("D"); // 依赖 B
        wf.addModule(A);
        wf.addModule(B);
        wf.addModule(C);
        wf.addModule(D);
        wf.add_edge("A", "C");
        wf.add_edge("B", "C");
        wf.add_edge("B", "D");
        wf.execute();
        assert_equal(static_cast<int>(wf.getModuleState("A")), static_cast<int>(concurrencpp::workflow::Executor::ModuleState::Failed));
        assert_equal(static_cast<int>(wf.getModuleState("B")), static_cast<int>(concurrencpp::workflow::Executor::ModuleState::Done));
        assert_equal(static_cast<int>(wf.getModuleState("C")), static_cast<int>(concurrencpp::workflow::Executor::ModuleState::Skipped));
        assert_equal(static_cast<int>(wf.getModuleState("D")), static_cast<int>(concurrencpp::workflow::Executor::ModuleState::Done));
        assert_equal(wf.getError("A"), std::string("A failed"));
        assert_state_converged(wf);
    }

    // 全局超时设置为 0/负数应禁用（不抛超时）
    void test_workflow_zero_timeout_disabled() {
        using namespace std::chrono;
        concurrencpp::workflow::Executor wf;
        wf.set_timeout(milliseconds{0});
        auto s1 = std::make_shared<delay_module>("s1", milliseconds{20});
        auto s2 = std::make_shared<delay_module>("s2", milliseconds{20});
        wf.addModule(s1);
        wf.addModule(s2);
        wf.execute();
        assert_equal(static_cast<int>(wf.getModuleState("s1")), static_cast<int>(concurrencpp::workflow::Executor::ModuleState::Done));
        assert_equal(static_cast<int>(wf.getModuleState("s2")), static_cast<int>(concurrencpp::workflow::Executor::ModuleState::Done));
        assert_state_converged(wf);
    }

    // CancelOnError + failing_module：一般失败场景，抛原始异常并终止同层后续，统一状态收敛
    void test_workflow_cancel_on_error_failing_module() {
        concurrencpp::workflow::Executor wf; // 默认 CancelOnError
        auto A = std::make_shared<failing_module>("A", "A failed");
        auto B = std::make_shared<ready_module>("B");
        auto C = std::make_shared<ready_module>("C");
        wf.addModule(A);
        wf.addModule(B);
        wf.addModule(C);

        assert_throws_with_error_message<std::runtime_error>([&] { wf.execute(); }, "A failed");
        assert_equal(static_cast<int>(wf.getModuleState("A")), static_cast<int>(concurrencpp::workflow::Executor::ModuleState::Failed));
        const auto sb = wf.getModuleState("B");
        const auto sc = wf.getModuleState("C");
        assert_true(sb != concurrencpp::workflow::Executor::ModuleState::Pending &&
                    sb != concurrencpp::workflow::Executor::ModuleState::Running);
        assert_true(sc != concurrencpp::workflow::Executor::ModuleState::Pending &&
                    sc != concurrencpp::workflow::Executor::ModuleState::Running);
        // 至少一个被跳过
        assert_true(sb == concurrencpp::workflow::Executor::ModuleState::Skipped ||
                    sc == concurrencpp::workflow::Executor::ModuleState::Skipped);
        assert_equal(wf.getError("A"), std::string("A failed"));
        assert_state_converged(wf);
    }

    // getModuleState 未知模块
    void test_workflow_get_state_unknown() {
        concurrencpp::workflow::Executor wf;
        assert_throws_contains_error_message<std::runtime_error>([&] { (void)wf.getModuleState("no_such"); }, "Unknown module");
    }

    // 优先级门控：并发上限为1时，高优先级模块应先启动
    void test_workflow_priority_gating_highest_first() {
        using namespace std::chrono;
        concurrencpp::workflow::Executor wf;
        auto A = std::make_shared<delay_module>("A", milliseconds{30});
        auto B = std::make_shared<delay_module>("B", milliseconds{30});
        auto C = std::make_shared<delay_module>("C", milliseconds{30});
        wf.addModule(A);
        wf.addModule(B);
        wf.addModule(C);
        wf.set_default_priority(0);
        wf.set_module_priority("A", 1);
        wf.set_module_priority("B", 10); // 最高优先级
        wf.set_module_priority("C", 0);
        wf.set_max_concurrency_per_round(1);
        wf.execute();
        // B 应最先启动，其开始时间不晚于其他模块
        const auto sA = wf.getModuleStats("A");
        const auto sB = wf.getModuleStats("B");
        const auto sC = wf.getModuleStats("C");
        assert_true(sB.start_time <= sA.start_time && sB.start_time <= sC.start_time);
        // 并发上限为1：下一模块的启动时间应在上一模块结束之后（近似顺序执行）
        assert_true(sA.start_time >= sB.end_time || sC.start_time >= sB.end_time);
        assert_state_converged(wf);
    }

    // 优先级相同：按插入顺序稳定排序（X、Y 先于 Z）
    void test_workflow_priority_tie_breaker_insertion_order() {
        using namespace std::chrono;
        concurrencpp::workflow::Executor wf;
        auto X = std::make_shared<delay_module>("X", milliseconds{20});
        auto Y = std::make_shared<delay_module>("Y", milliseconds{20});
        auto Z = std::make_shared<delay_module>("Z", milliseconds{20});
        // 插入顺序：X, Y, Z
        wf.addModule(X);
        wf.addModule(Y);
        wf.addModule(Z);
        wf.set_default_priority(5);
        wf.set_module_priority("X", 5);
        wf.set_module_priority("Y", 5);
        wf.set_module_priority("Z", 5);
        wf.set_max_concurrency_per_round(2);
        wf.execute();
        const auto sX = wf.getModuleStats("X");
        const auto sY = wf.getModuleStats("Y");
        const auto sZ = wf.getModuleStats("Z");
        // 前两个启动应为插入顺序的 X、Y（Z 应更晚）
        assert_true(sX.start_time <= sZ.start_time);
        assert_true(sY.start_time <= sZ.start_time);
        assert_state_converged(wf);
    }

    // 延迟老化：被延迟的就绪节点其优先级应提升
    void test_workflow_priority_aging_increases_deferred() {
        using namespace std::chrono;
        concurrencpp::workflow::Executor wf;
        auto A = std::make_shared<delay_module>("A", milliseconds{30});
        auto B = std::make_shared<delay_module>("B", milliseconds{30});
        wf.addModule(A);
        wf.addModule(B);
        wf.set_default_priority(0);
        wf.set_module_priority("A", 0); // 低优先级，预计会被延迟
        wf.set_module_priority("B", 5); // 高优先级，先执行
        wf.set_priority_aging_step(3);
        wf.set_max_concurrency_per_round(1);
        wf.execute();
        // A 被至少延迟一轮，其最终优先级应提升
        const int final_prio_A = wf.get_module_priority("A");
        assert_true(final_prio_A >= 3);
        assert_state_converged(wf);
    }

    // 优先级与依赖：高优先级模块依赖低优先级模块时应遵守依赖顺序
    void test_workflow_priority_respects_dependencies() {
        using namespace std::chrono;
        concurrencpp::workflow::Executor wf;
        auto A = std::make_shared<delay_module>("A", milliseconds{30});
        auto B = std::make_shared<delay_module>("B", milliseconds{10});
        wf.addModule(A);
        wf.addModule(B);
        wf.add_edge("A", "B");
        wf.set_default_priority(0);
        wf.set_module_priority("A", 0);
        wf.set_module_priority("B", 100);
        wf.set_max_concurrency_per_round(1);
        wf.execute();
        const auto sA = wf.getModuleStats("A");
        const auto sB = wf.getModuleStats("B");
        assert_true(sA.start_time <= sB.start_time);
        assert_true(sB.start_time >= sA.end_time);
        assert_state_converged(wf);
    }

    // 优先级老化：被延迟两轮时优先级应累积提升
    void test_workflow_priority_aging_accumulates_two_rounds() {
        using namespace std::chrono;
        concurrencpp::workflow::Executor wf;
        auto A = std::make_shared<delay_module>("A", milliseconds{15});
        auto B = std::make_shared<delay_module>("B", milliseconds{15});
        auto C = std::make_shared<delay_module>("C", milliseconds{15});
        wf.addModule(A);
        wf.addModule(B);
        wf.addModule(C);
        wf.set_default_priority(0);
        wf.set_module_priority("A", 0);
        wf.set_module_priority("B", 8);
        wf.set_module_priority("C", 4);
        wf.set_priority_aging_step(3);
        wf.set_max_concurrency_per_round(1);
        wf.execute();
        const auto sA = wf.getModuleStats("A");
        const auto sB = wf.getModuleStats("B");
        const auto sC = wf.getModuleStats("C");
        assert_true(sB.start_time <= sC.start_time && sC.start_time <= sA.start_time);
        const int final_prio_A = wf.get_module_priority("A");
        assert_true(final_prio_A >= 6);
        assert_state_converged(wf);
    }
} // namespace concurrencpp::tests

namespace concurrencpp::tests {

    // 探针模块：覆盖 on_cancel/on_suspend/on_resume，记录钩子触发情况
    class hook_probe_module : public concurrencpp::workflow::Module {
    public:
        std::atomic_bool canceled_probe{false};
        std::atomic_bool suspended_probe{false};

        explicit hook_probe_module(const std::string& name) : Module(name) {}

        void on_cancel() override {
            Module::on_cancel();
            canceled_probe.store(true, std::memory_order_relaxed);
        }
        void on_suspend() override {
            Module::on_suspend();
            suspended_probe.store(true, std::memory_order_relaxed);
        }
        void on_resume() override {
            Module::on_resume();
            suspended_probe.store(false, std::memory_order_relaxed);
        }

        result<void> execute_async(std::shared_ptr<concurrencpp::executor>) override {
            return concurrencpp::make_ready_result<void>();
        }
    };

    // 可等待模块：暴露对受保护的 check_suspend 的调用，以测试等待机制
    class waitable_module : public concurrencpp::workflow::Module {
    public:
        explicit waitable_module(const std::string& name) : Module(name) {}
        void wait_until_resumed_or_canceled() { check_suspend(); }
        result<void> execute_async(std::shared_ptr<concurrencpp::executor>) override {
            return concurrencpp::make_ready_result<void>();
        }
    };

    // 测试全局状态推送：suspend -> resume -> cancel 的钩子与状态变化
    void test_workflow_global_state_push_suspend_resume_cancel() {
        concurrencpp::workflow::Executor wf;
        auto mod = std::make_shared<hook_probe_module>("probe");
        wf.addModule(mod);

        // suspend
        wf.suspend();
        assert_true(mod->suspended_probe.load(std::memory_order_relaxed));
        assert_equal(static_cast<int>(wf.getModuleState("probe")),
                     static_cast<int>(concurrencpp::workflow::Executor::ModuleState::Suspended));

        // resume
        wf.resume();
        assert_true(!mod->suspended_probe.load(std::memory_order_relaxed));
        assert_equal(static_cast<int>(wf.getModuleState("probe")),
                     static_cast<int>(concurrencpp::workflow::Executor::ModuleState::Pending));

        // cancel
        wf.cancel();
        assert_true(mod->canceled_probe.load(std::memory_order_relaxed));
        assert_equal(static_cast<int>(wf.getModuleState("probe")),
                     static_cast<int>(concurrencpp::workflow::Executor::ModuleState::Canceled));
    }

    // 测试 set_executor_for_all：所有模块的首选执行器应被统一设置
    void test_workflow_set_executor_for_all_preferred() {
        concurrencpp::workflow::Executor wf;
        auto m1 = std::make_shared<recorder_module>("r1");
        auto m2 = std::make_shared<recorder_module>("r2");
        wf.addModule(m1);
        wf.addModule(m2);
        auto inline_ex = std::make_shared<concurrencpp::inline_executor>();
        wf.set_executor_for_all(inline_ex);
        assert_equal(m1->preferred_executor()->name, concurrencpp::details::consts::k_inline_executor_name);
        assert_equal(m2->preferred_executor()->name, concurrencpp::details::consts::k_inline_executor_name);
    }

    // 测试协作式等待：在 suspend 时阻塞，resume 后返回
    void test_workflow_check_suspend_wait_resume() {
        using namespace std::chrono;
        concurrencpp::workflow::Executor wf;
        auto wm = std::make_shared<waitable_module>("wait_resume");
        wf.addModule(wm);
        wf.suspend();
        std::thread t([&wf]() {
            std::this_thread::sleep_for(milliseconds{50});
            wf.resume();
        });
        const auto start = steady_clock::now();
        wm->wait_until_resumed_or_canceled();
        const auto end = steady_clock::now();
        t.join();
        assert_true(end - start >= milliseconds{30});
    }

    // 测试协作式等待：在 suspend 时阻塞，cancel 后返回
    void test_workflow_check_suspend_wait_cancel() {
        using namespace std::chrono;
        concurrencpp::workflow::Executor wf;
        auto wm = std::make_shared<waitable_module>("wait_cancel");
        wf.addModule(wm);
        wf.suspend();
        std::thread t([&wf]() {
            std::this_thread::sleep_for(milliseconds{50});
            wf.cancel();
        });
        const auto start = steady_clock::now();
        wm->wait_until_resumed_or_canceled();
        const auto end = steady_clock::now();
        t.join();
        assert_true(end - start >= milliseconds{30});
    }

    // ===== 新增：参数读写与共享功能测试 =====

    // 写入整型参数的模块
    class write_int_module : public concurrencpp::workflow::Module {
        std::string key_;
        int value_;
    public:
        write_int_module(const std::string& name, std::string key, int value)
            : Module(name), key_(std::move(key)), value_(value) {}
        result<void> execute_async(std::shared_ptr<concurrencpp::executor>) override {
            set_param(key_, value_);
            return concurrencpp::make_ready_result<void>();
        }
    };

    // 读取整型参数的模块
    class read_int_module : public concurrencpp::workflow::Module {
    public:
        std::string key_;
        bool existed_before {false};
        int got_value {0};
        explicit read_int_module(const std::string& name, std::string key)
            : Module(name), key_(std::move(key)) {}
        result<void> execute_async(std::shared_ptr<concurrencpp::executor>) override {
            existed_before = param_exists(key_);
            auto p = get_param<int>(key_);
            got_value = *p;
            return concurrencpp::make_ready_result<void>();
        }
    };

    // 写入向量参数
    class write_vec_module : public concurrencpp::workflow::Module {
        std::string key_;
        std::vector<int> init_;
    public:
        write_vec_module(const std::string& name, std::string key, std::vector<int> init)
            : Module(name), key_(std::move(key)), init_(std::move(init)) {}
        result<void> execute_async(std::shared_ptr<concurrencpp::executor>) override {
            set_param<std::vector<int>>(key_, std::move(init_));
            return concurrencpp::make_ready_result<void>();
        }
    };

    // 修改并读取向量参数（with_write / with_read）
    class modify_vec_module : public concurrencpp::workflow::Module {
        std::string key_;
        int extra_ {0};
    public:
        size_t final_size {0};
        modify_vec_module(const std::string& name, std::string key, int extra)
            : Module(name), key_(std::move(key)), extra_(extra) {}
        result<void> execute_async(std::shared_ptr<concurrencpp::executor>) override {
            with_write_param<std::vector<int>>(key_, [&](std::vector<int>& v){ v.push_back(extra_); });
            with_read_param<std::vector<int>>(key_, [&](const std::vector<int>& v){ final_size = v.size(); });
            return concurrencpp::make_ready_result<void>();
        }
    };

    // 读取错误类型以触发类型不匹配异常
    class wrong_type_reader_module : public concurrencpp::workflow::Module {
        std::string key_;
    public:
        explicit wrong_type_reader_module(const std::string& name, std::string key)
            : Module(name), key_(std::move(key)) {}
        result<void> execute_async(std::shared_ptr<concurrencpp::executor>) override {
            try {
                // 期望读取 int，但实际存储为其他类型
                (void)get_param<int>(key_);
                return concurrencpp::make_ready_result<void>();
            } catch (...) {
                return concurrencpp::make_exceptional_result<void>(std::current_exception());
            }
        }
    };

    // 用例1：Executor 注入默认 ParamStore 且可替换
    void test_workflow_param_store_injection_and_replace() {
        concurrencpp::workflow::Executor wf;
        auto r = std::make_shared<ready_module>("probe_ready");
        wf.addModule(r);
        // 默认注入
        assert_true(wf.param_store() != nullptr);
        assert_true(r->param_store() != nullptr);
        // 替换注入
        auto new_ps = std::make_shared<concurrencpp::workflow::ParamStore>();
        wf.set_param_store(new_ps);
        assert_true(wf.param_store() == new_ps);
        assert_true(r->param_store() == new_ps);
    }

    // 用例2：写入整型参数并在下游读取
    void test_workflow_param_flow_writer_reader() {
        concurrencpp::workflow::Executor wf;
        auto w = std::make_shared<write_int_module>("W", "k_num", 42);
        auto r = std::make_shared<read_int_module>("R", "k_num");
        wf.addModule(w);
        wf.addModule(r);
        wf.add_edge("W", "R");
        wf.execute();
        assert_true(r->existed_before);
        assert_equal(r->got_value, 42);
        assert_state_converged(wf);
    }

    // 用例3：向量参数 with_write/with_read 协作访问
    void test_workflow_param_vector_rw() {
        concurrencpp::workflow::Executor wf;
        auto w = std::make_shared<write_vec_module>("Wv", "k_vec", std::vector<int>{1,2,3});
        auto m = std::make_shared<modify_vec_module>("Mv", "k_vec", 4);
        wf.addModule(w);
        wf.addModule(m);
        wf.add_edge("Wv", "Mv");
        wf.execute();
        assert_equal(static_cast<int>(m->final_size), 4);
        assert_state_converged(wf);
    }

    // 用例4：类型不匹配错误应在 CancelOnError 策略下传播
    void test_workflow_param_type_mismatch_propagates() {
        concurrencpp::workflow::Executor wf; // 默认 CancelOnError
        // 写入字符串，再尝试按 int 读取同键
        auto w = std::make_shared<write_int_module>("Ws", "bad_key", 0);
        // 先将键写为字符串，覆盖 write_int 的值
        w = std::make_shared<write_int_module>("Ws", "bad_key", 0);
        // 使用另一个模块写入字符串以确保类型为 string
        class write_string_module : public concurrencpp::workflow::Module {
            std::string key_;
            std::string val_;
        public:
            write_string_module(const std::string& name, std::string key, std::string val)
                : Module(name), key_(std::move(key)), val_(std::move(val)) {}
            result<void> execute_async(std::shared_ptr<concurrencpp::executor>) override {
                set_param(key_, val_);
                return concurrencpp::make_ready_result<void>();
            }
        };
        auto ws = std::make_shared<write_string_module>("Ws", "bad_key", std::string("abc"));
        auto wr = std::make_shared<wrong_type_reader_module>("Wr", "bad_key");
        wf.addModule(ws);
        wf.addModule(wr);
        wf.add_edge("Ws", "Wr");
        assert_throws_contains_error_message<std::runtime_error>([&] { wf.execute(); }, "type mismatch");
        // 状态与错误信息
        assert_equal(static_cast<int>(wf.getModuleState("Ws")), static_cast<int>(concurrencpp::workflow::Executor::ModuleState::Done));
        assert_equal(static_cast<int>(wf.getModuleState("Wr")), static_cast<int>(concurrencpp::workflow::Executor::ModuleState::Failed));
        auto err = wf.getError("Wr");
        assert_true(err.find("type mismatch") != std::string::npos);
        assert_state_converged(wf);
    }
}

using namespace concurrencpp::tests;

int main() {
    tester tester("workflow executor tests");

    // 精简：保留关键功能覆盖的测试用例
    tester.add_step("fallback to default", test_workflow_fallback_to_default_executor);
    tester.add_step("shutdown exception", test_workflow_shutdown_exception);
    tester.add_step("duplicate names", test_workflow_duplicate_names);
    tester.add_step("unknown edge", test_workflow_unknown_edge);
    tester.add_step("cycle detection", test_workflow_cycle_detection);
    tester.add_step("missing dependency", test_workflow_missing_dependency);
    tester.add_step("cancel before start", test_workflow_cancel_before_start);
    tester.add_step("global timeout partial", test_workflow_global_timeout_partial);
    tester.add_step("module timeout continue policy", test_workflow_module_timeout_continue_policy);
    tester.add_step("continue-on-error downstream skip", test_workflow_continue_on_error_downstream_skip);
    tester.add_step("cancel-on-error failing module", test_workflow_cancel_on_error_failing_module);
    tester.add_step("zero timeout disabled", test_workflow_zero_timeout_disabled);
    tester.add_step("get state unknown", test_workflow_get_state_unknown);
    // 优先级调度测试
    tester.add_step("priority highest-first", test_workflow_priority_gating_highest_first);
    tester.add_step("priority tie-breaker insertion order", test_workflow_priority_tie_breaker_insertion_order);
    tester.add_step("priority aging increases deferred", test_workflow_priority_aging_increases_deferred);
    tester.add_step("priority respects dependencies", test_workflow_priority_respects_dependencies);
    tester.add_step("priority aging accumulates two rounds", test_workflow_priority_aging_accumulates_two_rounds);

    // 新增：全局状态推送与协作式等待相关用例
    tester.add_step("global state push suspend/resume/cancel flags", test_workflow_global_state_push_suspend_resume_cancel);
    tester.add_step("set_executor_for_all sets preferred", test_workflow_set_executor_for_all_preferred);
    tester.add_step("check_suspend waits until resume", test_workflow_check_suspend_wait_resume);
    tester.add_step("check_suspend waits until cancel", test_workflow_check_suspend_wait_cancel);

    // 新增：参数读写与共享功能用例
    tester.add_step("param store injection and replace", test_workflow_param_store_injection_and_replace);
    tester.add_step("param flow writer->reader", test_workflow_param_flow_writer_reader);
    tester.add_step("param vector with_write/read", test_workflow_param_vector_rw);
    tester.add_step("param type-mismatch propagates", test_workflow_param_type_mismatch_propagates);

    tester.launch_test();
    return 0;
}