# Workflow 模块 API 文档

本模块位于命名空间 `concurrencpp::workflow`，基于 DAG（有向无环图）对模块进行并行调度与依赖管理，采用 ConcurrenCPP 的执行器与 `result`/`lazy_result` 协程结果类型。

## 术语与核心类型

- `Module`：工作流中的基础模块，表示一个可执行的任务单元，支持异步执行并返回 `std::any` 结果。
- `TypedModule<T>`：带类型的模块基类，执行返回 `T` 类型结果，并自动桥接到 `std::any`。
- `Executor`：工作流执行器，负责依赖分析、并行调度、错误与性能统计、图分析以及导出。
- `ModulePtr`：`std::shared_ptr<Module>` 别名。

## Module（基础模块）

头文件：`include/concurrencpp/workflow/module.h`

- 构造：`Module(const std::string& name, const std::vector<std::string>& depend = {})`
  - `name` 模块名称（唯一），`depend` 依赖的模块名称列表。
- 名称：`const std::string& getName() const`
- 依赖：
  - `const std::vector<std::string>& getDepend() const`
  - `const std::vector<std::string>& getDependencies() const`（`getDepend` 的直观别名）
- 依赖结果访问（执行期间由 Executor 注入）：
  - `void setDependencyResults(std::unordered_map<std::string, std::any>* results)`（内部使用）
  - `std::any getDependencyResult(const std::string& module_name) const`
  - `template<typename T> T getDependencyResult(const std::string& module_name) const`
- 执行：
  - 纯虚：`result<std::any> execute_async(std::shared_ptr<concurrencpp::executor> executor) = 0`
  - 同步包装：`void execute(std::shared_ptr<concurrencpp::executor> executor = nullptr)`

## TypedModule<T>（泛型模块）

- 继承：`class TypedModule<T> : public Module`
- 主要接口：
  - `virtual result<T> execute_typed(std::shared_ptr<concurrencpp::executor> executor) = 0`
  - 桥接：`result<std::any> execute_async(...)` 内部调用 `execute_typed`，并将 `T` 包装为 `std::any`。
- `void` 特化：`class TypedModule<void> : public Module`
  - `virtual result<void> execute_typed(...) = 0`
  - `execute_async(...)` 完成后返回空 `std::any`。

## Executor（执行器）

头文件：`include/concurrencpp/workflow/executor.h`

### 构造与模块管理

- 构造：
  - `explicit Executor(std::shared_ptr<concurrencpp::executor> executor)`（自定义执行器）
  - `Executor()`（默认使用线程池执行器）
- 添加模块：`void addModule(std::shared_ptr<Module> module)`
- 清理：`void clear()`（释放模块与结果、重置图缓存与统计）
- 查询：
  - `size_t getModuleCount() const`
  - `bool hasModule(const std::string& module_name) const`
  - `std::vector<std::string> getModuleNames() const`

### 执行与状态

- 异步执行：`lazy_result<void> execute_async()`（并行启动所有无依赖的根模块，递归执行其他模块）
- 同步执行：`void execute()`（阻塞等待 `execute_async` 完成）
- 状态枚举：`enum class ExecutionStatus { NOT_STARTED, RUNNING, COMPLETED, FAILED }`
- 状态查询：
  - `ExecutionStatus getStatus() const`
  - `bool hasFailed() const`
  - `std::vector<ModuleError> getErrors() const`（`ModuleError{ module_name, error_message, timestamp }`）
  - `std::chrono::milliseconds getExecutionDuration() const`
  - `void resetStatus()`（重置到 `NOT_STARTED` 并清空错误与结果）

### 结果访问

- 任意类型：`std::any getModuleResult(const std::string& module_name) const`
- 强类型：`template<typename T> T getModuleResult(const std::string& module_name) const`
  - 若 `T` 为 `void`，则返回时无值。
  - 若模块失败或未执行，抛异常。

### 性能与内存统计

- 单模块：`const ModuleStats& getModuleStats(const std::string& module_name) const`
  - 字段：`module_name`、`start_time`、`end_time`、`execution_duration`、`memory_usage_bytes`、`completed_successfully`
- 全局：`const WorkflowStats& getWorkflowStats() const`
  - 字段：`total_modules`、`completed_modules`、`failed_modules`、`total_execution_time`、`average_module_time`、`peak_memory_usage`、`cpu_utilization`
- 聚合：`std::vector<ModuleStats> getAllModuleStats() const`
- 重置：`void resetStats()`
- 内存估计：
  - `size_t getCurrentMemoryUsage() const`
  - `size_t getPeakMemoryUsage() const`

### 观察钩子（可选）

- `void set_on_start(std::function<void(const std::string&)> cb)`
- `void set_on_complete(std::function<void(const ModuleStats&)> cb)`
- `void set_on_error(std::function<void(const ModuleError&)> cb)`

### 进度与导出

- 进度：
  - `double getProgressPercent() const`（按完成模块数估算）
  - `size_t getCompletedCount() const`
- 导出：
  - `std::string export_dot() const`（导出 DAG 为 DOT）
  - `std::string export_csv_stats() const`（导出模块统计为 CSV）

### 图分析（只读）

- `std::vector<std::string> topo_order() const`（Kahn 算法）
- `std::vector<std::vector<std::string>> compute_levels() const`（层级/波前计算）
- `bool detect_cycle(std::vector<std::string>* cycle_out = nullptr) const`（DFS 检测）
- `std::pair<long long, std::vector<std::string>> critical_path(const std::unordered_map<std::string, long long>* weights = nullptr) const`
- `size_t max_parallel_width() const`
- `unsigned long long count_paths() const`

## 使用示例（简版）

```cpp
using namespace concurrencpp::workflow;

class A : public TypedModule<int> {
public:
  using TypedModule::TypedModule;
  concurrencpp::result<int> execute_typed(std::shared_ptr<concurrencpp::executor>) override {
    auto task = [](std::shared_ptr<concurrencpp::executor>) -> concurrencpp::lazy_result<int> { co_return 42; };
    return task(nullptr).run();
  }
};

class B : public TypedModule<int> {
public:
  using TypedModule::TypedModule;
  concurrencpp::result<int> execute_typed(std::shared_ptr<concurrencpp::executor>) override {
    auto task = [this](std::shared_ptr<concurrencpp::executor>) -> concurrencpp::lazy_result<int> {
      int a = this->getDependencyResult<int>("A");
      co_return a * 2;
    };
    return task(nullptr).run();
  }
};

Executor wf;
wf.addModule(std::make_shared<A>("A"));
wf.addModule(std::make_shared<B>("B", std::vector<std::string>{"A"}));
wf.execute();
int b = wf.getModuleResult<int>("B");
```

## 注意事项与最佳实践

- 模块名称需唯一，依赖引用通过名称匹配。
- `getDependencyResult<T>` 仅在模块执行期间有效（由执行器注入结果指针）。
- 图分析接口为只读，不依赖执行是否完成；若存在环，会返回相应提示（例如关键路径返回空或负值）。
- 观察钩子可能在多线程环境中触发，回调内尽量保持轻量与线程安全。
- 失败模块的结果访问会抛异常；可通过 `getErrors()` 查看失败详情。