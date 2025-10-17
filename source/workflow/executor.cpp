#include "concurrencpp/workflow/executor.h"
#include "concurrencpp/workflow/module.h"
#include "concurrencpp/concurrencpp.h"
#include "concurrencpp/timers/timer_queue.h"

#include <deque>
#include <algorithm>

using namespace concurrencpp::workflow;

Executor::Executor(std::shared_ptr<concurrencpp::executor> executor)
    : m_runtime(std::make_shared<concurrencpp::runtime>()), m_executor(std::move(executor)) {}

Executor::Executor() : Executor(std::shared_ptr<concurrencpp::executor>()) {
    if (!m_executor) {
        m_executor = m_runtime->thread_pool_executor();
    }
}


void Executor::addModule(std::shared_ptr<Module> module) {
    const auto& name = module->getName();
    if (m_modules.count(name)) {
        throw std::runtime_error("Duplicate module name: " + name);
    }
    // 注入 runtime 供模块选择执行器/创建资源
    module->setRuntime(m_runtime);
    m_modules.emplace(name, std::move(module));
    // 初始化状态
    m_states[name] = ModuleState::Pending;
    // 记录插入顺序，保证同层处理的确定性
    m_order.push_back(name);
}

void Executor::add_edge(const std::string& from, const std::string& to) {
    auto it_to = m_modules.find(to);
    auto it_from = m_modules.find(from);
    if (it_from == m_modules.end() || it_to == m_modules.end()) {
        throw std::runtime_error("add_edge: unknown module(s): " + from + " -> " + to);
    }
    it_to->second->addDepend(from);
}

static void check_cycle(const std::unordered_map<std::string, std::shared_ptr<Module>>& modules) {
    enum class Mark { None, Temp, Perm };
    std::unordered_map<std::string, Mark> mark;
    for (const auto& kv : modules) mark[kv.first] = Mark::None;

    std::function<void(const std::string&)> visit = [&](const std::string& u) {
        if (mark[u] == Mark::Perm) return;
        if (mark[u] == Mark::Temp) throw std::runtime_error("Circular dependency at: " + u);
        mark[u] = Mark::Temp;
        auto it = modules.find(u);
        if (it == modules.end()) throw std::runtime_error("Unknown node in cycle check: " + u);
        for (const auto& v : it->second->getDepend()) {
            if (!modules.count(v)) throw std::runtime_error("Missing dependency: " + v + " for module: " + u);
            visit(v);
        }
        mark[u] = Mark::Perm;
    };
    for (const auto& kv : modules) visit(kv.first);
}

// 废弃递归与串行模式，统一采用批并行拓扑

concurrencpp::lazy_result<void> Executor::run_topo_batch() {
    check_cycle(m_modules);
    // 每次执行前重置状态与统计
    m_errors.clear();
    m_module_stats.clear();
    for (const auto& [name, _] : m_modules) {
        m_states[name] = ModuleState::Pending;
    }
    m_deferred_rounds.clear();
    // 记录工作流开始时间
    m_workflow_stats.start_time = std::chrono::steady_clock::now();
    // 构建索引化的局部图结构：入度与邻接，减少哈希与字符串操作
    const size_t N = m_modules.size();
    std::vector<std::string> names;
    names.reserve(N);
    if (m_order.size() == N) {
        names = m_order;
    } else {
        for (const auto& [name, _] : m_modules) names.emplace_back(name);
    }
    std::unordered_map<std::string, size_t> index_of;
    index_of.reserve(N);
    for (size_t i = 0; i < names.size(); ++i) index_of.emplace(names[i], i);

    // 插入顺序索引，用于优先级相同情况下的稳定排序
    std::unordered_map<std::string, size_t> order_index;
    order_index.reserve(m_order.size());
    for (size_t i = 0; i < m_order.size(); ++i) order_index.emplace(m_order[i], i);

    std::vector<int> indeg(N, 0);
    std::vector<int> failed_dep_count(N, 0);
    std::vector<size_t> outdeg(N, 0);
    std::vector<std::vector<size_t>> adj(N);
    // 统计入度与出度
    for (const auto& [name, mod] : m_modules) {
        const size_t u = index_of.at(name);
        for (const auto& dep : mod->getDepend()) {
            const size_t v = index_of.at(dep);
            indeg[u]++;
            outdeg[v]++;
        }
    }
    // 为邻接表预留容量并填充
    for (size_t i = 0; i < N; ++i) adj[i].reserve(outdeg[i]);
    for (const auto& [name, mod] : m_modules) {
        const size_t u = index_of.at(name);
        for (const auto& dep : mod->getDepend()) {
            const size_t v = index_of.at(dep);
            adj[v].push_back(u);
        }
    }

    // 端到端全局超时预算：在开始时计算统一的 deadline
    std::chrono::steady_clock::time_point deadline;
    if (has_global_timeout()) {
        deadline = std::chrono::steady_clock::now() + m_timeout;
    }

    std::deque<size_t> q;
    for (size_t i = 0; i < N; ++i) if (indeg[i] == 0) q.push_back(i);

    // 复用层向量与结果向量以减少重复分配
    std::vector<size_t> layer_idx;
    layer_idx.reserve(N);
    std::vector<size_t> run_layer_idx;
    run_layer_idx.reserve(N);
    std::vector<concurrencpp::result<void>> results;
    results.reserve(N);
    std::vector<concurrencpp::shared_result<void>> shared_results;
    shared_results.reserve(N);

    while (!q.empty()) {
        // 检查取消（可能由外部或超时触发）
        if (m_cancel.load(std::memory_order_relaxed)) {
            // 对所有尚未完成（Pending/Running）的模块触发协作式取消，并标记为 Skipped
            for (auto& [name, st] : m_states) {
                if (st == ModuleState::Pending || st == ModuleState::Running) {
                    auto it = m_modules.find(name);
                    if (it != m_modules.end() && it->second) {
                        it->second->on_cancel();
                    }
                    st = ModuleState::Skipped;
                }
            }
            throw concurrencpp::errors::interrupted_task("Workflow canceled or timed out");
        }
        // collect current layer
        layer_idx.clear();
        const size_t layer_size = q.size();
        layer_idx.reserve(layer_size);
        for (size_t i = 0; i < layer_size; ++i) {
            layer_idx.push_back(q.front());
            q.pop_front();
        }

        // Partition into runnable candidates and skipped (has failed deps)
        run_layer_idx.clear();
        run_layer_idx.reserve(layer_idx.size());
        std::vector<size_t> skip_layer_idx;
        skip_layer_idx.reserve(layer_idx.size());
        std::vector<size_t> runnable_candidates;
        runnable_candidates.reserve(layer_idx.size());
        results.clear();
        for (const auto& u : layer_idx) {
            const auto& name = names[u];
            if (failed_dep_count[u] > 0) {
                m_states[name] = ModuleState::Skipped;
                skip_layer_idx.push_back(u);
                continue;
            }
            runnable_candidates.push_back(u);
        }

        // Apply dynamic priority gating if needed
        std::vector<size_t> selected_run_idx;
        selected_run_idx.reserve(runnable_candidates.size());
        std::vector<size_t> deferred_idx;
        deferred_idx.reserve(runnable_candidates.size());
        if (!m_has_max_concurrency || m_max_concurrency_per_round == 0 || runnable_candidates.size() <= m_max_concurrency_per_round) {
            selected_run_idx = std::move(runnable_candidates);
        } else {
            auto prio_of = [this, &names](size_t idx) {
                const auto& nm = names[idx];
                auto it = m_priorities.find(nm);
                return (it != m_priorities.end()) ? it->second : m_default_priority;
            };
            std::stable_sort(runnable_candidates.begin(), runnable_candidates.end(), [&](size_t a, size_t b) {
                const int pa = prio_of(a);
                const int pb = prio_of(b);
                if (pa != pb) return pa > pb; // higher priority first
                const auto& na = names[a];
                const auto& nb = names[b];
                const size_t oa = (order_index.count(na) ? order_index.at(na) : a);
                const size_t ob = (order_index.count(nb) ? order_index.at(nb) : b);
                return oa < ob; // earlier insertion first
            });
            const size_t limitK = std::min(m_max_concurrency_per_round, runnable_candidates.size());
            selected_run_idx.assign(runnable_candidates.begin(), runnable_candidates.begin() + limitK);
            deferred_idx.assign(runnable_candidates.begin() + limitK, runnable_candidates.end());
        }

        // Prepare results for selected nodes; age priorities and requeue deferred nodes
        results.reserve(selected_run_idx.size());
        for (const auto& u : selected_run_idx) {
            const auto& name = names[u];
            auto mod = m_modules.at(name);
            m_states[name] = ModuleState::Running;
            // 记录开始时间
            m_module_stats[name].start_time = std::chrono::steady_clock::now();
            auto ex = mod->select_executor(m_runtime);
            if (!ex) ex = m_executor;
            auto mod_res = mod->execute_async(ex);
            auto per_timeout = mod->timeout();
            if (per_timeout.count() > 0) {
                auto tq = m_runtime->timer_queue();
                auto delay_lazy = tq->make_delay_object(per_timeout, ex);
                auto composed = [this, delay_lazy = std::move(delay_lazy), mod_res = std::move(mod_res)]() mutable -> concurrencpp::lazy_result<void> {
                    auto any = co_await concurrencpp::when_any(m_executor, delay_lazy.run(), std::move(mod_res));
                    if (any.index == 0) {
                        throw concurrencpp::errors::interrupted_task("Module timed out");
                    }
                    co_await std::get<1>(any.results);
                    co_return;
                };
                results.emplace_back(composed().run());
            } else {
                results.emplace_back(std::move(mod_res));
            }
            run_layer_idx.push_back(u);
        }

        for (const auto& u : deferred_idx) {
            const auto& name = names[u];
            // aging and defer
            m_deferred_rounds[name] += 1;
            m_priorities[name] = get_module_priority(name) + m_aging_step;
            // keep state pending and requeue for next round
            m_states[name] = ModuleState::Pending;
            q.push_back(u);
        }
        // 为避免在竞速过程中消耗原始 result，这里统一转换为 shared_result 进行等待与查询
        shared_results.clear();
        shared_results.reserve(results.size());
        for (auto& r : results) {
            shared_results.emplace_back(std::move(r));
        }

        // await all (race with global timeout if configured),使用剩余时间
        if (has_global_timeout()) {
            const auto now = std::chrono::steady_clock::now();
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            if (remaining.count() <= 0) {
                // 已无剩余时间：设置取消并标记当前层
                m_cancel.store(true, std::memory_order_relaxed);
                for (size_t i = 0; i < shared_results.size(); ++i) {
                    const auto st = shared_results[i].status();
                    // 注意：shared_results 与 run_layer_idx 对应（仅包含可运行节点）
                    const auto& name = names[run_layer_idx[i]];
                    if (st == concurrencpp::result_status::value) {
                        m_states[name] = ModuleState::Done;
                        auto& stats = m_module_stats[name];
                        stats.end_time = std::chrono::steady_clock::now();
                        stats.duration = std::chrono::duration_cast<std::chrono::milliseconds>(stats.end_time - stats.start_time);
                    } else if (st == concurrencpp::result_status::exception) {
                        m_states[name] = ModuleState::Failed;
                        try {
                            shared_results[i].get();
                        } catch (const std::exception& e) {
                            m_errors[name] = e.what();
                        } catch (...) {
                            m_errors[name] = "Unknown error";
                        }
                        auto& stats = m_module_stats[name];
                        stats.end_time = std::chrono::steady_clock::now();
                        stats.duration = std::chrono::duration_cast<std::chrono::milliseconds>(stats.end_time - stats.start_time);
                    } else {
                        // 尚未完成：协作式取消
                        auto it = m_modules.find(name);
                        if (it != m_modules.end() && it->second) {
                            it->second->on_cancel();
                        }
                        m_states[name] = ModuleState::Skipped;
                        auto& stats = m_module_stats[name];
                        // 标记结束（无耗时）
                        stats.end_time = stats.start_time;
                        stats.duration = std::chrono::milliseconds{0};
                    }
                }
                throw concurrencpp::errors::interrupted_task("Workflow canceled or timed out");
            }

            auto tq = m_runtime->timer_queue();
            auto timeout_lazy = tq->make_delay_object(remaining, m_executor);
            auto any = co_await concurrencpp::when_any(
                m_executor,
                wait_all_nonthrowing(shared_results).run(),
                timeout_lazy.run());
            if (any.index == 1) {
                // 超时：更新已完成/异常的状态，未完成标记为 Skipped
                m_cancel.store(true, std::memory_order_relaxed);
                for (size_t i = 0; i < shared_results.size(); ++i) {
                    const auto st = shared_results[i].status();
                    const auto& name = names[run_layer_idx[i]];
                    if (st == concurrencpp::result_status::value) {
                        m_states[name] = ModuleState::Done;
                        auto& stats = m_module_stats[name];
                        stats.end_time = std::chrono::steady_clock::now();
                        stats.duration = std::chrono::duration_cast<std::chrono::milliseconds>(stats.end_time - stats.start_time);
                    } else if (st == concurrencpp::result_status::exception) {
                        m_states[name] = ModuleState::Failed;
                        try {
                            shared_results[i].get();
                        } catch (const std::exception& e) {
                            m_errors[name] = e.what();
                        } catch (...) {
                            m_errors[name] = "Unknown error";
                        }
                        auto& stats = m_module_stats[name];
                        stats.end_time = std::chrono::steady_clock::now();
                        stats.duration = std::chrono::duration_cast<std::chrono::milliseconds>(stats.end_time - stats.start_time);
                    } else {
                        // 尚未完成：协作式取消
                        auto it = m_modules.find(name);
                        if (it != m_modules.end() && it->second) {
                            it->second->on_cancel();
                        }
                        m_states[name] = ModuleState::Skipped;
                        auto& stats = m_module_stats[name];
                        stats.end_time = stats.start_time;
                        stats.duration = std::chrono::milliseconds{0};
                    }
                }
                throw concurrencpp::errors::interrupted_task("Workflow canceled or timed out");
            }
        } else {
            co_await wait_all_nonthrowing(shared_results);
        }
        // 将索引化的本层可运行节点转回名称以处理结果
        std::vector<std::string> run_layer_names;
        run_layer_names.reserve(run_layer_idx.size());
        for (auto idx : run_layer_idx) run_layer_names.emplace_back(names[idx]);
        process_layer_results(shared_results, run_layer_names);

        // relax edges: 仅对已处理的节点（selected + skipped）进行边松弛；延迟节点保持就绪以待下一轮。
        for (const auto& u : run_layer_idx) {
            const bool u_failed = (m_states[names[u]] == ModuleState::Failed || m_states[names[u]] == ModuleState::Skipped);
            for (const auto& v : adj[u]) {
                if (u_failed) {
                    failed_dep_count[v]++;
                }
                if (--indeg[v] == 0) {
                    // 始终入队，在下一轮统一决定是否执行或跳过，以便继续松弛其后继边
                    q.push_back(v);
                }
            }
        }
        // 对于因依赖失败被跳过的节点，同样进行边松弛，以便传播失败影响
        for (const auto& u : skip_layer_idx) {
            const bool u_failed = (m_states[names[u]] == ModuleState::Failed || m_states[names[u]] == ModuleState::Skipped);
            for (const auto& v : adj[u]) {
                if (u_failed) {
                    failed_dep_count[v]++;
                }
                if (--indeg[v] == 0) {
                    q.push_back(v);
                }
            }
        }
    }
    // 记录工作流结束时间与耗时，并触发完成回调
    m_workflow_stats.end_time = std::chrono::steady_clock::now();
    m_workflow_stats.duration = std::chrono::duration_cast<std::chrono::milliseconds>(m_workflow_stats.end_time - m_workflow_stats.start_time);
    co_return;
}

// 无递归执行版本

concurrencpp::lazy_result<void> Executor::execute_async() {
    co_await run_topo_batch();
}

void Executor::execute() {
    execute_async().run().get();
}

// 移除执行模式重载

void Executor::clear() {
    m_modules.clear();
    m_states.clear();
    m_errors.clear();
    m_order.clear();
    m_cancel.store(false, std::memory_order_relaxed);
}

size_t Executor::getModuleCount() const { return m_modules.size(); }

bool Executor::hasModule(const std::string& module_name) const { return m_modules.count(module_name) != 0; }

std::vector<std::string> Executor::getModuleNames() const {
    std::vector<std::string> names;
    names.reserve(m_modules.size());
    for (const auto& [name, _] : m_modules) names.push_back(name);
    return names;
}

Executor::ModuleState Executor::getModuleState(const std::string& module_name) const {
    auto it = m_states.find(module_name);
    if (it == m_states.end()) {
        throw std::runtime_error("Unknown module: " + module_name);
    }
    return it->second;
}

std::unordered_map<std::string, Executor::ModuleState> Executor::getAllStates() const {
    return m_states;
}

std::vector<std::string> Executor::getFailedModules() const {
    std::vector<std::string> failed;
    for (const auto& [name, st] : m_states) {
        if (st == ModuleState::Failed) failed.push_back(name);
    }
    return failed;
}

std::string Executor::getError(const std::string& module_name) const {
    auto it = m_errors.find(module_name);
    if (it == m_errors.end()) return {};
    return it->second;
}
concurrencpp::lazy_result<void> Executor::wait_all_nonthrowing(std::vector<concurrencpp::shared_result<void>>& shared_results) {
    // 仅等待所有共享结果变为“已解析”，不提取值以避免异常在此处传播。
    for (auto& sr : shared_results) {
        co_await sr.resolve();
    }
    // 切回调度执行器，以确保后续处理在一致的上下文中进行。
    co_await concurrencpp::resume_on(m_executor);
}

void Executor::process_layer_results(std::vector<concurrencpp::shared_result<void>>& shared_results,
                                     const std::vector<std::string>& layer) {
    if (m_error_policy == ErrorPolicy::CancelOnError) {
        for (size_t i = 0; i < shared_results.size(); ++i) {
            try {
                shared_results[i].get();
                // 记录模块结束时间与耗时
                auto& stats = m_module_stats[layer[i]];
                stats.end_time = std::chrono::steady_clock::now();
                stats.duration = std::chrono::duration_cast<std::chrono::milliseconds>(stats.end_time - stats.start_time);
            } catch (const std::exception& e) {
                // 设置取消标志，便于下游模块/用户代码检测到取消意图
                m_cancel.store(true, std::memory_order_relaxed);
                m_states[layer[i]] = ModuleState::Failed;
                m_errors[layer[i]] = e.what();
                auto& stats_i = m_module_stats[layer[i]];
                stats_i.end_time = std::chrono::steady_clock::now();
                stats_i.duration = std::chrono::duration_cast<std::chrono::milliseconds>(stats_i.end_time - stats_i.start_time);
                for (size_t j = i + 1; j < shared_results.size(); ++j) {
                    // 协作式取消后续同层模块
                    auto it = m_modules.find(layer[j]);
                    if (it != m_modules.end() && it->second) {
                        it->second->on_cancel();
                    }
                    m_states[layer[j]] = ModuleState::Skipped;
                    // 记录被跳过模块的结束时间（与开始时间相同，耗时为0）
                    auto& s = m_module_stats[layer[j]];
                    s.end_time = s.start_time;
                    s.duration = std::chrono::milliseconds{0};
                }
                throw; // 停止后续层
            } catch (...) {
                m_cancel.store(true, std::memory_order_relaxed);
                m_states[layer[i]] = ModuleState::Failed;
                m_errors[layer[i]] = "Unknown error";
                auto& stats_i = m_module_stats[layer[i]];
                stats_i.end_time = std::chrono::steady_clock::now();
                stats_i.duration = std::chrono::duration_cast<std::chrono::milliseconds>(stats_i.end_time - stats_i.start_time);
                for (size_t j = i + 1; j < shared_results.size(); ++j) {
                    auto it = m_modules.find(layer[j]);
                    if (it != m_modules.end() && it->second) {
                        it->second->on_cancel();
                    }
                    m_states[layer[j]] = ModuleState::Skipped;
                    auto& s = m_module_stats[layer[j]];
                    s.end_time = s.start_time;
                    s.duration = std::chrono::milliseconds{0};
                }
                throw;
            }
            m_states[layer[i]] = ModuleState::Done;
        }
    } else { // ContinueOnError
        for (size_t i = 0; i < shared_results.size(); ++i) {
            try {
                shared_results[i].get();
                m_states[layer[i]] = ModuleState::Done;
                auto& stats = m_module_stats[layer[i]];
                stats.end_time = std::chrono::steady_clock::now();
                stats.duration = std::chrono::duration_cast<std::chrono::milliseconds>(stats.end_time - stats.start_time);
            } catch (const std::exception& e) {
                m_states[layer[i]] = ModuleState::Failed;
                m_errors[layer[i]] = e.what();
                auto& stats = m_module_stats[layer[i]];
                stats.end_time = std::chrono::steady_clock::now();
                stats.duration = std::chrono::duration_cast<std::chrono::milliseconds>(stats.end_time - stats.start_time);
            } catch (...) {
                m_states[layer[i]] = ModuleState::Failed;
                m_errors[layer[i]] = "Unknown error";
                auto& stats = m_module_stats[layer[i]];
                stats.end_time = std::chrono::steady_clock::now();
                stats.duration = std::chrono::duration_cast<std::chrono::milliseconds>(stats.end_time - stats.start_time);
            }
        }
    }
}

void Executor::pick_by_priority_and_gate(
    const std::vector<size_t>& runnable_candidates,
    const std::vector<std::string>& names,
    const std::unordered_map<std::string, size_t>& order_index,
    std::vector<size_t>& selected_run_idx,
    std::vector<size_t>& deferred_idx) const {
    selected_run_idx.clear();
    deferred_idx.clear();
    if (!m_has_max_concurrency || m_max_concurrency_per_round == 0 || runnable_candidates.size() <= m_max_concurrency_per_round) {
        selected_run_idx = runnable_candidates;
        return;
    }
    auto prio_of = [this, &names](size_t idx) {
        const auto& nm = names[idx];
        auto it = m_priorities.find(nm);
        return (it != m_priorities.end()) ? it->second : m_default_priority;
    };
    std::vector<size_t> sorted = runnable_candidates;
    std::stable_sort(sorted.begin(), sorted.end(), [&](size_t a, size_t b) {
        const int pa = prio_of(a);
        const int pb = prio_of(b);
        if (pa != pb) return pa > pb; // higher priority first
        const auto& na = names[a];
        const auto& nb = names[b];
        const size_t oa = (order_index.count(na) ? order_index.at(na) : a);
        const size_t ob = (order_index.count(nb) ? order_index.at(nb) : b);
        return oa < ob; // earlier insertion first
    });
    const size_t limitK = std::min(m_max_concurrency_per_round, sorted.size());
    selected_run_idx.assign(sorted.begin(), sorted.begin() + limitK);
    deferred_idx.assign(sorted.begin() + limitK, sorted.end());
}

void Executor::age_and_requeue_deferred(
    const std::vector<size_t>& deferred_idx,
    const std::vector<std::string>& names,
    std::deque<size_t>& q) {
    for (const auto& u : deferred_idx) {
        const auto& name = names[u];
        m_deferred_rounds[name] += 1;
        m_priorities[name] = get_module_priority(name) + m_aging_step;
        m_states[name] = ModuleState::Pending;
        q.push_back(u);
    }
}

#if 0
std::string Executor::export_dot() const {
    // 生成 GraphViz dot 表示，包含节点与依赖边
    std::string dot = "digraph workflow {\n";
    // 节点
    for (const auto& [name, mod] : m_modules) {
        (void)mod;
        dot += "  \"" + name + "\"";
        // 附加状态作为标签
        auto it = m_states.find(name);
        if (it != m_states.end()) {
            dot += " [label=\"" + name + " (" + std::to_string(static_cast<int>(it->second)) + ")\"]";
        }
        dot += ";\n";
    }
    // 边
    for (const auto& [name, mod] : m_modules) {
        for (const auto& dep : mod->getDepend()) {
            dot += "  \"" + dep + "\" -> \"" + name + "\";\n";
        }
    }
    dot += "}\n";
    return dot;
}
#endif