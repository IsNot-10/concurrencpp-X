#ifndef CONCURRENCPP_WORKFLOW_EXECUTOR_H
#define CONCURRENCPP_WORKFLOW_EXECUTOR_H

#include "concurrencpp/results/lazy_result.h"
#include "concurrencpp/results/shared_result.h"
#include "concurrencpp/executors/executor.h"
#include "concurrencpp/runtime/runtime.h"
#include "module.h"
#include "param.h"
#include <unordered_map>
#include <vector>
#include <memory>
#include <string>
#include <chrono>
#include <stdexcept>
#include <atomic>
#include <functional>
#include <deque>

namespace concurrencpp::workflow {
    class Module;

    class Executor {
    public:
        // 对外公开的状态与错误策略枚举，便于测试与用户代码引用
        enum class ModuleState { Pending, Running, Done, Failed, Skipped, Suspended, Canceled, Timeout };
        enum class ErrorPolicy { CancelOnError, ContinueOnError };
        enum class TimeoutPolicy { AsError, AsNormal };

        // 统计结构体（可选查询）
        struct ModuleStats {
            std::chrono::steady_clock::time_point start_time{};
            std::chrono::steady_clock::time_point end_time{};
            std::chrono::milliseconds duration{0};
        };

        struct WorkflowStats {
            std::chrono::steady_clock::time_point start_time{};
            std::chrono::steady_clock::time_point end_time{};
            std::chrono::milliseconds duration{0};
        };

    private:
        // 轻量图结构，统一拓扑构建的中间表示
        struct Graph {
            size_t N{0};
            std::vector<std::string> names;
            std::unordered_map<std::string, size_t> index_of;
            std::unordered_map<std::string, size_t> order_index;
            std::vector<int> indeg;
            std::vector<int> failed_dep_count;
            std::vector<std::vector<size_t>> adj;
        };

        Graph build_graph() const;
        void apply_timeout_effect(std::vector<concurrencpp::shared_result<void>>& shared_results,
                                  const std::vector<size_t>& run_layer_idx,
                                  const std::vector<std::string>& names);
        void relax_edges(const std::vector<size_t>& u_list,
                         const std::vector<std::string>& names,
                         const std::vector<std::vector<size_t>>& adj,
                         std::vector<int>& indeg,
                         std::vector<int>& failed_dep_count,
                         std::deque<size_t>& q);

        std::unordered_map<std::string, std::shared_ptr<Module>> m_modules;
        std::shared_ptr<concurrencpp::runtime> m_runtime;
        std::shared_ptr<concurrencpp::executor> m_executor; // 默认使用 runtime 的线程池
        std::shared_ptr<ParamStore> m_param_store; // 全局共享参数存储

        std::unordered_map<std::string, ModuleState> m_states;
        std::unordered_map<std::string, std::string> m_errors;
        // 记录模块插入顺序，保证同层处理的确定性
        std::vector<std::string> m_order;

        // 错误策略：失败时的处理方式
        ErrorPolicy m_error_policy = ErrorPolicy::CancelOnError;
        TimeoutPolicy m_timeout_policy = TimeoutPolicy::AsError;

        // 取消与超时支持
        std::atomic_bool m_cancel {false};
        bool m_has_timeout {false};
        std::chrono::milliseconds m_timeout {0};

        // 统计信息
        WorkflowStats m_workflow_stats{};
        std::unordered_map<std::string, ModuleStats> m_module_stats;

        // 动态优先级调度配置与状态
        std::unordered_map<std::string, int> m_priorities; // 模块当前优先级
        std::unordered_map<std::string, size_t> m_deferred_rounds; // 在就绪层中被延迟的轮数
        int m_default_priority {0};
        int m_aging_step {1}; // 每次延迟增加的优先级步长
        bool m_has_max_concurrency {false};
        size_t m_max_concurrency_per_round {0}; // 每轮最多并发的模块数（0 表示不限）

        // 可观测性钩子
        std::function<void(const std::string&)> m_on_start;
        std::function<void(const std::string&)> m_on_complete;
        std::function<void(const std::string&, const std::string&)> m_on_error;

        // 内部拓扑批量执行（同层并行）
        lazy_result<void> run_topo_batch();

        // 统一的超时判断，减少重复代码
        inline bool has_global_timeout() const noexcept {
            return m_has_timeout && m_timeout.count() > 0;
        }

        // 等待辅助：非消耗、无异常传播的等待全部 shared_result 完成
        lazy_result<void> wait_all_nonthrowing(std::vector<concurrencpp::shared_result<void>>& shared_results);

        // 处理当前层的结果，依据错误策略更新状态与错误
        void process_layer_results(std::vector<concurrencpp::shared_result<void>>& shared_results,
                                   const std::vector<std::string>& layer);

        // 优先级门控：根据优先级和插入顺序从候选中选择运行与延迟集合
        void pick_by_priority_and_gate(
            const std::vector<size_t>& runnable_candidates,
            const std::vector<std::string>& names,
            const std::unordered_map<std::string, size_t>& order_index,
            std::vector<size_t>& selected_run_idx,
            std::vector<size_t>& deferred_idx) const;

        // 老化并重新排队延迟节点
        void age_and_requeue_deferred(
            const std::vector<size_t>& deferred_idx,
            const std::vector<std::string>& names,
            std::deque<size_t>& q);

    public:
        explicit Executor(std::shared_ptr<concurrencpp::executor> executor);
        Executor();

        void addModule(std::shared_ptr<Module> module);

        // 便捷添加依赖关系：将 B 依赖 A
        void add_edge(const std::string& from, const std::string& to);

        // 统一默认执行入口：拓扑并行（同层并行）
        lazy_result<void> execute_async();
        void execute();

        // void-only 模式下不再提供结果查询接口

        void clear();
        size_t getModuleCount() const;
        bool hasModule(const std::string& module_name) const;
        std::vector<std::string> getModuleNames() const;

        // 状态与错误查询接口
        ModuleState getModuleState(const std::string& module_name) const;
        std::unordered_map<std::string, ModuleState> getAllStates() const;
        std::vector<std::string> getFailedModules() const;
        std::string getError(const std::string& module_name) const;

        // 统计接口
        WorkflowStats getWorkflowStats() const { return m_workflow_stats; }
        ModuleStats getModuleStats(const std::string& module_name) const {
            auto it = m_module_stats.find(module_name);
            if (it == m_module_stats.end()) {
                throw std::runtime_error("Unknown module: " + module_name);
            }
            return it->second;
        }
        std::unordered_map<std::string, ModuleStats> getAllModuleStats() const { return m_module_stats; }

        // 可观测性钩子设置
        void set_on_start(std::function<void(const std::string&)> cb) { m_on_start = std::move(cb); }
        void set_on_complete(std::function<void(const std::string&)> cb) { m_on_complete = std::move(cb); }
        void set_on_error(std::function<void(const std::string&, const std::string&)> cb) { m_on_error = std::move(cb); }

        // 错误策略与取消/超时控制
        void set_error_policy(ErrorPolicy policy) { m_error_policy = policy; }
        ErrorPolicy get_error_policy() const { return m_error_policy; }

        void set_timeout_policy(TimeoutPolicy policy) { m_timeout_policy = policy; }
        TimeoutPolicy get_timeout_policy() const { return m_timeout_policy; }

        void set_timeout(std::chrono::milliseconds timeout) {
            if (timeout.count() <= 0) {
                m_has_timeout = false;
                m_timeout = std::chrono::milliseconds {0};
            } else {
                m_has_timeout = true;
                m_timeout = timeout;
            }
        }

        void request_cancel() { m_cancel.store(true, std::memory_order_relaxed); }
        bool cancel_requested() const { return m_cancel.load(std::memory_order_relaxed); }

        // 动态优先级接口
        void set_module_priority(const std::string& module_name, int priority) { m_priorities[module_name] = priority; }
        int get_module_priority(const std::string& module_name) const {
            auto it = m_priorities.find(module_name);
            if (it != m_priorities.end()) return it->second;
            return m_default_priority;
        }
        void set_default_priority(int prio) { m_default_priority = prio; }
        void set_priority_aging_step(int step) { m_aging_step = step; }
        void set_max_concurrency_per_round(size_t max_concurrency) {
            if (max_concurrency == 0) {
                m_has_max_concurrency = false;
                m_max_concurrency_per_round = 0;
            } else {
                m_has_max_concurrency = true;
                m_max_concurrency_per_round = max_concurrency;
            }
        }

        // 线程池设置
        void set_default_executor(std::shared_ptr<concurrencpp::executor> ex) { m_executor = std::move(ex); }
        void set_executor_for_all(std::shared_ptr<concurrencpp::executor> ex) {
            for (auto& kv : m_modules) {
                if (kv.second) {
                    kv.second->setPreferredExecutor(ex);
                }
            }
        }

        // 参数存储设置/访问：用于外部预置参数或替换实现
        void set_param_store(std::shared_ptr<ParamStore> ps) {
            m_param_store = std::move(ps);
            for (auto& kv : m_modules) {
                if (kv.second) {
                    kv.second->setParamStore(m_param_store);
                }
            }
        }
        std::shared_ptr<ParamStore> param_store() const { return m_param_store; }

        // 全局状态推送：暂停/恢复/取消
        void push_all_state(ModuleState state) {
            switch (state) {
                case ModuleState::Suspended:
                    for (auto& kv : m_modules) {
                        if (kv.second) {
                            kv.second->on_suspend();
                            m_states[kv.first] = ModuleState::Suspended;
                        }
                    }
                    break;
                case ModuleState::Canceled:
                    for (auto& kv : m_modules) {
                        if (kv.second) {
                            kv.second->on_cancel();
                            // 不覆盖已完成模块状态
                            auto it = m_states.find(kv.first);
                            if (it == m_states.end() || it->second != ModuleState::Done) {
                                m_states[kv.first] = ModuleState::Canceled;
                            }
                        }
                    }
                    break;
                case ModuleState::Pending: // 作为 Resume 语义
                    for (auto& kv : m_modules) {
                        if (kv.second) {
                            kv.second->on_resume();
                            // 从 Suspended 恢复到 Pending
                            auto it = m_states.find(kv.first);
                            if (it == m_states.end() || it->second == ModuleState::Suspended) {
                                m_states[kv.first] = ModuleState::Pending;
                            }
                        }
                    }
                    break;
                default:
                    // 其他状态不做全局推送
                    break;
            }
        }
        void suspend() { push_all_state(ModuleState::Suspended); }
        void resume() { push_all_state(ModuleState::Pending); }
        void cancel() { request_cancel(); push_all_state(ModuleState::Canceled); }

    };

} // namespace concurrencpp::workflow

#endif // CONCURRENCPP_WORKFLOW_EXECUTOR_H