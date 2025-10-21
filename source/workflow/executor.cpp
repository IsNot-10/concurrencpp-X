#include "concurrencpp/workflow/executor.h"
#include "concurrencpp/concurrencpp.h"
#include "concurrencpp/timers/timer_queue.h"

#include <deque>
#include <algorithm>

using namespace concurrencpp::workflow;

Executor::Executor(std::shared_ptr<concurrencpp::executor> executor)
    : m_runtime(std::make_shared<concurrencpp::runtime>()), m_executor(std::move(executor)) {
    // 初始化全局参数存储
    if (!m_param_store) {
        m_param_store = std::make_shared<ParamStore>();
    }
}

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
    // 注入共享参数存储
    if (m_param_store) {
        module->setParamStore(m_param_store);
    }
    m_modules.emplace(name, std::move(module));
    // 初始化连续存储索引与状态
    size_t idx = m_module_data.size();
    m_name_to_index[name] = idx;
    m_module_data.emplace_back(ModuleData{});
    m_module_data[idx].state = ModuleState::Pending;
    m_module_data[idx].priority = m_default_priority;
    m_module_data[idx].deferred_rounds = 0;
    // 记录插入顺序，保证同层处理的确定性
    m_order.push_back(name);
    // 图结构已变化，缓存失效
    m_graph_cache_valid = false;
}

void Executor::add_edge(const std::string& from, const std::string& to) {
    auto it_to = m_modules.find(to);
    auto it_from = m_modules.find(from);
    if (it_from == m_modules.end() || it_to == m_modules.end()) {
        throw std::runtime_error("add_edge: unknown module(s): " + from + " -> " + to);
    }
    // 移除环检测与自依赖限制，交由用户保证拓扑正确性
    it_to->second->addDepend(from);
    // 依赖变化，图缓存失效
    m_graph_cache_valid = false;
}

// 移除静态的循环检测函数，运行期在拓扑过程中由缺失依赖检查保障基本一致性

// 废弃递归与串行模式，统一采用批并行拓扑

concurrencpp::lazy_result<void> Executor::run_topo_batch() {
    // 移除预运行环检测，直接进入批量拓扑执行
    // 每次执行前重置状态与统计
    m_errors.clear();
    for (auto& md : m_module_data) {
        md.state = ModuleState::Pending;
        md.stats = ModuleStats{};
        md.deferred_rounds = 0;
    }
    // 记录工作流开始时间
    m_workflow_stats.start_time = std::chrono::steady_clock::now();
    // 构建统一图结构（复用缓存）
    const auto& g = build_graph();
    const size_t N = g.N;
    const auto& names = g.names;
    auto indeg = g.indeg;
    auto failed_dep_count = g.failed_dep_count;
    const auto& adj_data = g.adj_data;
    const auto& adj_offset = g.adj_offset;

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
        if (m_cancel.load(std::memory_order_relaxed)) {
            for (size_t i = 0; i < m_module_data.size(); ++i) {
                auto& md = m_module_data[i];
                if (md.state == ModuleState::Pending || md.state == ModuleState::Running) {
                    const auto& name = names[i];
                    auto it = m_modules.find(name);
                    if (it != m_modules.end() && it->second) {
                        it->second->on_cancel();
                    }
                    md.state = ModuleState::Skipped;
                }
            }
            throw concurrencpp::errors::interrupted_task("Workflow canceled or timed out");
        }
        layer_idx.clear();
        const size_t layer_size = q.size();
        layer_idx.reserve(layer_size);
        for (size_t i = 0; i < layer_size; ++i) {
            layer_idx.push_back(q.front());
            q.pop_front();
        }

        run_layer_idx.clear();
        run_layer_idx.reserve(layer_idx.size());
        std::vector<size_t> skip_layer_idx;
        skip_layer_idx.reserve(layer_idx.size());
        std::vector<size_t> runnable_candidates;
        runnable_candidates.reserve(layer_idx.size());
        results.clear();
        for (const auto& u : layer_idx) {
            if (failed_dep_count[u] > 0) {
                m_module_data[u].state = ModuleState::Skipped;
                skip_layer_idx.push_back(u);
                continue;
            }
            runnable_candidates.push_back(u);
        }

        std::vector<size_t> selected_run_idx;
        selected_run_idx.reserve(runnable_candidates.size());
        std::vector<size_t> deferred_idx;
        deferred_idx.reserve(runnable_candidates.size());
        pick_by_priority_and_gate(runnable_candidates, names, selected_run_idx, deferred_idx);

        results.reserve(selected_run_idx.size());
        for (const auto& u : selected_run_idx) {
            const auto& name = names[u];
            auto mod = m_modules.at(name);
            m_module_data[u].state = ModuleState::Running;
            m_module_data[u].stats.start_time = std::chrono::steady_clock::now();
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

        age_and_requeue_deferred(deferred_idx, names, q);

        shared_results.clear();
        shared_results.reserve(results.size());
        for (auto& r : results) {
            shared_results.emplace_back(std::move(r));
        }

        if (has_global_timeout()) {
            const auto now = std::chrono::steady_clock::now();
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            if (remaining.count() <= 0) {
                apply_timeout_effect(shared_results, run_layer_idx, names);
                throw concurrencpp::errors::interrupted_task("Workflow canceled or timed out");
            }
            auto tq = m_runtime->timer_queue();
            auto timeout_lazy = tq->make_delay_object(remaining, m_executor);
            auto any = co_await concurrencpp::when_any(
                m_executor,
                wait_all_nonthrowing(shared_results).run(),
                timeout_lazy.run());
            if (any.index == 1) {
                apply_timeout_effect(shared_results, run_layer_idx, names);
                throw concurrencpp::errors::interrupted_task("Workflow canceled or timed out");
            }
        } else {
            co_await wait_all_nonthrowing(shared_results);
        }

        process_layer_results(shared_results, run_layer_idx, names);
        relax_edges(run_layer_idx, names, adj_data, adj_offset, indeg, failed_dep_count, q);
        relax_edges(skip_layer_idx, names, adj_data, adj_offset, indeg, failed_dep_count, q);
    }

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
    m_module_data.clear();
    m_name_to_index.clear();
    m_errors.clear();
    m_order.clear();
    m_cancel.store(false, std::memory_order_relaxed);
    // 使图缓存失效并清空
    m_graph_cache_valid = false;
    m_graph_cache = Graph{};
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
    auto it = m_name_to_index.find(module_name);
    if (it == m_name_to_index.end()) {
        throw std::runtime_error("Unknown module: " + module_name);
    }
    return m_module_data[it->second].state;
}

std::unordered_map<std::string, Executor::ModuleState> Executor::getAllStates() const {
    std::unordered_map<std::string, Executor::ModuleState> out;
    for (const auto& name : m_order) {
        auto it = m_name_to_index.find(name);
        if (it != m_name_to_index.end()) {
            out.emplace(name, m_module_data[it->second].state);
        }
    }
    return out;
}

std::vector<std::string> Executor::getFailedModules() const {
    std::vector<std::string> failed;
    for (const auto& name : m_order) {
        auto it = m_name_to_index.find(name);
        if (it != m_name_to_index.end()) {
            if (m_module_data[it->second].state == ModuleState::Failed) failed.push_back(name);
        }
    }
    return failed;
}

std::string Executor::getError(const std::string& module_name) const {
    auto it = m_errors.find(module_name);
    if (it != m_errors.end()) {
        return it->second;
    }
    return std::string{};
}

// Batch state update to reduce cache-line bouncing
void Executor::batch_update_states(const std::vector<size_t>& indices, ModuleState new_state) {
    for (auto idx : indices) {
        if (idx < m_module_data.size()) {
            m_module_data[idx].state = new_state;
        }
    }
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
                                     const std::vector<size_t>& layer_idx,
                                     const std::vector<std::string>& names) {
    if (m_error_policy == ErrorPolicy::CancelOnError) {
        for (size_t i = 0; i < shared_results.size(); ++i) {
            const size_t u = layer_idx[i];
            const auto& nm = names[u];
            try {
                shared_results[i].get();
                // 记录模块结束时间与耗时
                auto& stats = m_module_data[u].stats;
                stats.end_time = std::chrono::steady_clock::now();
                stats.duration = std::chrono::duration_cast<std::chrono::milliseconds>(stats.end_time - stats.start_time);
            } catch (const std::exception& e) {
                // 设置取消标志，便于下游模块/用户代码检测到取消意图
                m_cancel.store(true, std::memory_order_relaxed);
                m_module_data[u].state = ModuleState::Failed;
                m_errors[nm] = e.what();
                auto& stats_i = m_module_data[u].stats;
                stats_i.end_time = std::chrono::steady_clock::now();
                stats_i.duration = std::chrono::duration_cast<std::chrono::milliseconds>(stats_i.end_time - stats_i.start_time);
                for (size_t j = i + 1; j < shared_results.size(); ++j) {
                    const size_t uj = layer_idx[j];
                    const auto& nj = names[uj];
                    // 协作式取消后续同层模块
                    auto it = m_modules.find(nj);
                    if (it != m_modules.end() && it->second) {
                        it->second->on_cancel();
                    }
                    m_module_data[uj].state = ModuleState::Skipped;
                    // 记录被跳过模块的结束时间（与开始时间相同，耗时为0）
                    auto& s = m_module_data[uj].stats;
                    s.end_time = s.start_time;
                    s.duration = std::chrono::milliseconds{0};
                }
                throw; // 停止后续层
            } catch (...) {
                m_cancel.store(true, std::memory_order_relaxed);
                m_module_data[u].state = ModuleState::Failed;
                m_errors[nm] = "Unknown error";
                auto& stats_i = m_module_data[u].stats;
                stats_i.end_time = std::chrono::steady_clock::now();
                stats_i.duration = std::chrono::duration_cast<std::chrono::milliseconds>(stats_i.end_time - stats_i.start_time);
                for (size_t j = i + 1; j < shared_results.size(); ++j) {
                    const size_t uj = layer_idx[j];
                    const auto& nj = names[uj];
                    auto it = m_modules.find(nj);
                    if (it != m_modules.end() && it->second) {
                        it->second->on_cancel();
                    }
                    m_module_data[uj].state = ModuleState::Skipped;
                    auto& s = m_module_data[uj].stats;
                    s.end_time = s.start_time;
                    s.duration = std::chrono::milliseconds{0};
                }
                throw;
            }
            m_module_data[u].state = ModuleState::Done;
        }
    } else { // ContinueOnError
        for (size_t i = 0; i < shared_results.size(); ++i) {
            const size_t u = layer_idx[i];
            const auto& nm = names[u];
            try {
                shared_results[i].get();
                m_module_data[u].state = ModuleState::Done;
                auto& stats = m_module_data[u].stats;
                stats.end_time = std::chrono::steady_clock::now();
                stats.duration = std::chrono::duration_cast<std::chrono::milliseconds>(stats.end_time - stats.start_time);
            } catch (const std::exception& e) {
                m_module_data[u].state = ModuleState::Failed;
                m_errors[nm] = e.what();
                auto& stats = m_module_data[u].stats;
                stats.end_time = std::chrono::steady_clock::now();
                stats.duration = std::chrono::duration_cast<std::chrono::milliseconds>(stats.end_time - stats.start_time);
            } catch (...) {
                m_module_data[u].state = ModuleState::Failed;
                m_errors[nm] = "Unknown error";
                auto& stats = m_module_data[u].stats;
                stats.end_time = std::chrono::steady_clock::now();
                stats.duration = std::chrono::duration_cast<std::chrono::milliseconds>(stats.end_time - stats.start_time);
            }
        }
    }
}

void Executor::pick_by_priority_and_gate(
    const std::vector<size_t>& runnable_candidates,
    const std::vector<std::string>& names,
    std::vector<size_t>& selected_run_idx,
    std::vector<size_t>& deferred_idx) const {
    selected_run_idx.clear();
    deferred_idx.clear();
    if (!m_has_max_concurrency || m_max_concurrency_per_round == 0 || runnable_candidates.size() <= m_max_concurrency_per_round) {
        selected_run_idx = runnable_candidates;
        return;
    }
    auto prio_of = [this](size_t idx) {
        return (idx < m_module_data.size()) ? m_module_data[idx].priority : m_default_priority;
    };
    auto cmp = [&](size_t a, size_t b) {
        const int pa = prio_of(a);
        const int pb = prio_of(b);
        if (pa != pb) return pa > pb; // higher priority first
        // 插入顺序打平：names 与 m_order 对齐时，索引即顺序
        return a < b;
    };
    std::vector<size_t> sorted = runnable_candidates;
    const size_t limitK = std::min(m_max_concurrency_per_round, sorted.size());
    // 通用方案：partial_sort 对前 K 完全排序，O(N log K)
    std::partial_sort(sorted.begin(), sorted.begin() + limitK, sorted.end(), cmp);
    selected_run_idx.assign(sorted.begin(), sorted.begin() + limitK);
    // 其余作为延迟集合，无需排序以减少开销
    deferred_idx.assign(sorted.begin() + limitK, sorted.end());
}

void Executor::age_and_requeue_deferred(
    const std::vector<size_t>& deferred_idx,
    const std::vector<std::string>& names,
    std::deque<size_t>& q) {
    for (const auto& u : deferred_idx) {
        const auto& name = names[u];
        m_module_data[u].deferred_rounds += 1;
        int base = get_module_priority(name);
        m_module_data[u].priority = base + m_aging_step;
        m_module_data[u].state = ModuleState::Pending;
        q.push_back(u);
    }
}

void Executor::relax_edges(
    const std::vector<size_t>& u_list,
    const std::vector<std::string>& names,
    const std::vector<size_t>& adj_data,
    const std::vector<size_t>& adj_offset,
    std::vector<int>& indeg,
    std::vector<int>& failed_dep_count,
    std::deque<size_t>& q) {
    for (const auto& u : u_list) {
        const bool u_failed = (m_module_data[u].state == ModuleState::Failed || m_module_data[u].state == ModuleState::Skipped);
        for (size_t i = adj_offset[u]; i < adj_offset[u + 1]; ++i) {
            const auto v = adj_data[i];
            if (u_failed) {
                failed_dep_count[v]++;
            }
            if (--indeg[v] == 0) {
                q.push_back(v);
            }
        }
    }
}

const Executor::Graph& Executor::build_graph() const {
    if (m_graph_cache_valid) {
        return m_graph_cache;
    }
    Graph g;
    g.N = m_modules.size();
    g.names.reserve(g.N);
    if (m_order.size() == g.N) {
        g.names = m_order; // 使用插入顺序，保证确定性
    } else {
        for (const auto& [name, _] : m_modules) g.names.emplace_back(name);
    }
    // 复制名称到索引的映射，避免重复字符串查找
    // index_of & order_index are not used; omit to reduce memory and build time

    g.indeg.assign(g.N, 0);
    g.failed_dep_count.assign(g.N, 0);

    // 统计每个节点的出度（指向其所有依赖者）
    std::vector<size_t> outdeg(g.N, 0);
    for (const auto& [name, mod] : m_modules) {
        const size_t u = m_name_to_index.at(name);
        for (const auto& dep : mod->getDepend()) {
            auto it_dep = m_name_to_index.find(dep);
            if (it_dep == m_name_to_index.end()) {
                throw std::runtime_error("Missing dependency: " + dep + " for module: " + name);
            }
            const size_t v = it_dep->second;
            g.indeg[u]++;
            outdeg[v]++;
        }
    }

    // 构建扁平化邻接结构
    g.adj_offset.resize(g.N + 1);
    g.adj_offset[0] = 0;
    for (size_t i = 0; i < g.N; ++i) {
        g.adj_offset[i + 1] = g.adj_offset[i] + outdeg[i];
    }
    g.adj_data.resize(g.adj_offset[g.N]);
    // 复用 outdeg 作为写指针，减少额外内存
    for (size_t i = 0; i < g.N; ++i) {
        outdeg[i] = g.adj_offset[i];
    }
    for (const auto& [name, mod] : m_modules) {
        const size_t u = m_name_to_index.at(name);
        for (const auto& dep : mod->getDepend()) {
            const size_t v = m_name_to_index.at(dep);
            g.adj_data[outdeg[v]++] = u; // v -> u（u 依赖 v）
        }
    }

    // 写入缓存并返回
    m_graph_cache = std::move(g);
    m_graph_cache_valid = true;
    return m_graph_cache;
}

void Executor::apply_timeout_effect(std::vector<concurrencpp::shared_result<void>>& shared_results,
                                    const std::vector<size_t>& run_layer_idx,
                                    const std::vector<std::string>& names) {
    m_cancel.store(true, std::memory_order_relaxed);
    for (size_t i = 0; i < shared_results.size(); ++i) {
        const auto st = shared_results[i].status();
        const size_t u = run_layer_idx[i];
        const auto& name = names[u];
        if (st == concurrencpp::result_status::value) {
            m_module_data[u].state = ModuleState::Done;
            auto& stats = m_module_data[u].stats;
            stats.end_time = std::chrono::steady_clock::now();
            stats.duration = std::chrono::duration_cast<std::chrono::milliseconds>(stats.end_time - stats.start_time);
        } else if (st == concurrencpp::result_status::exception) {
            m_module_data[u].state = ModuleState::Failed;
            try {
                shared_results[i].get();
            } catch (const std::exception& e) {
                m_errors[name] = e.what();
            } catch (...) {
                m_errors[name] = "Unknown error";
            }
            auto& stats = m_module_data[u].stats;
            stats.end_time = std::chrono::steady_clock::now();
            stats.duration = std::chrono::duration_cast<std::chrono::milliseconds>(stats.end_time - stats.start_time);
        } else {
            auto it = m_modules.find(name);
            if (it != m_modules.end() && it->second) {
                it->second->on_cancel();
            }
            m_module_data[u].state = ModuleState::Skipped;
            auto& stats = m_module_data[u].stats;
            stats.end_time = stats.start_time;
            stats.duration = std::chrono::milliseconds{0};
        }
    }
}