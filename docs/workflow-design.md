Workflow 模块源码设计文档

本设计文档从源码实现视角介绍 `concurrencpp::workflow` 模块的架构、关键数据结构与算法、执行与并发模型、错误与状态管理、图分析与导出、性能统计与观察钩子，以及扩展性与当前局限。该文档与《workflow 模块 API 文档》配套阅读。

总体架构
- 目标：在有向无环图（DAG）中按依赖并行地执行模块，提供稳定的执行状态、结果访问、错误与性能统计，以及基础的图分析与导出能力。
- 分层：
  - 接口层（头文件）：`include/concurrencpp/workflow/module.h`、`include/concurrencpp/workflow/executor.h`
  - 实现层（源文件）：`source/workflow/executor.cpp`（核心调度与统计逻辑）
  - 示例：`example/14_workflow_basic`、`example/15_workflow_features`、`example/16_workflow_analytics_demo`
- 职责划分：
  - `Module`/`TypedModule<T>`：定义模块单元、类型化执行接口、依赖结果访问；
  - `Executor`：管理模块集合与依赖图，负责并发调度、状态与错误、性能统计、图分析与导出。

关键类型与数据结构
- `Module`（抽象基类）：
  - 字段：模块名、依赖列表、执行期的依赖结果视图指针（指向执行器的结果表）。
  - 方法：
    - 元信息：`getName()`、`getDependencies()`；
    - 依赖结果访问：`getDependencyResult(name)` 与模板重载，在执行期读取依赖输出；
    - 执行接口：`execute_async(executor)` 纯虚，返回 `concurrencpp::result<std::any>`。
- `TypedModule<T>`（模板类）：
  - 定义类型化接口 `execute_typed(executor)`（纯虚，返回 `concurrencpp::result<T>`）；
  - 在 `execute_async` 中桥接到 `std::any`；`void` 特化返回空结果。
- `Executor`：
  - 模块与结果：`m_modules: unordered_map<string, shared_ptr<Module>>`，`m_results: unordered_map<string, any>`；
  - 状态与错误：`m_status: ExecutionStatus`、`m_errors: vector<ModuleError>`、开始/结束时间戳；
  - 统计：`m_module_stats: unordered_map<string, ModuleStats>`、`m_workflow_stats: WorkflowStats`、`m_peak_memory_usage`；
  - 图缓存：`m_adj_cache`（邻接表）、`m_indeg_cache`、`m_outdeg_cache` 与 `m_graph_dirty` 标记；
  - 观察钩子：`m_on_start`、`m_on_complete`、`m_on_error`。

执行流程与调度
- 入口：`execute_async(runtime, thread_pool_executor)`
  - 设置状态为 `RUNNING`，记录开始时间，清空历史错误与统计。
  - 为结果与统计容器预留容量，降低重分配开销。
  - 并发启动所有根模块（无依赖），对其它模块通过递归推进其依赖链。
  - 等待所有触达子图完成；若存在不连通子图则补齐执行。
  - 聚合统计并设置状态为 `COMPLETED` 或（有错误）`FAILED`，记录结束时间。
- 递归调度：`execute_module_recursive(name, visited, executing)`
  - 环检测：`executing` 维护当前递归栈，若入栈重复即判为环并抛异常。
  - 依赖执行：对每个依赖节点先递归执行，确保输入已就绪。
  - 依赖注入：模块开始前将其依赖结果视图指向执行器的 `m_results`。
  - 钩子与度量：触发 `m_on_start(name)`，执行模块 `execute_async`，记录开始/结束时间，粗略估算内存并写入 `m_module_stats`。
  - 完成写入：将输出写入 `m_results[name]`、更新 `visited`，触发 `m_on_complete(stats)`。
  - 异常路径：捕获异常，记录 `ModuleError`，将状态置为 `FAILED`，写入失败统计，触发 `m_on_error(err)` 并重新抛出。
- 同步封装：`execute(...)` 通过等待 `execute_async` 完成实现阻塞执行。

并发与线程安全
- 互斥分层：
  - 状态与错误：独立互斥保护 `m_status`、`m_errors`、起止时间。
  - 统计：独立互斥保护 `m_module_stats`、`m_workflow_stats`、峰值内存。
  - 图缓存：独立互斥保护 `m_adj_cache`、`m_indeg_cache`、`m_outdeg_cache` 与脏标记。
- 依赖注入时机：模块开始前设置视图指针，完成后清空以避免悬挂引用。
- 执行器与线程池：
  - 执行器使用外部传入的 `thread_pool_executor` 进行并发；自身不创建线程。
  - 并发写入通过细粒度锁保证可见性与一致性，减少锁竞争与死锁风险。

错误与状态管理
- 状态枚举：`ExecutionStatus { NOT_STARTED, RUNNING, COMPLETED, FAILED }`。
- 错误记录：模块异常构建 `ModuleError { name, message, timestamp }` 并写入队列。
- 对外查询与操作：
  - `getStatus()`、`hasFailed()`、`getErrors()` 查询状态与错误；
  - `getModuleResult(name)`（含模板）在失败或缺失时抛异常；
  - 进度：`getProgressPercent()`、`getCompletedCount()` 基于统计与节点数估算；
  - 复位/清理：`resetStatus()` 将状态设回 `NOT_STARTED`，`clear()` 清除模块、结果、缓存与统计。

图分析与缓存策略
- 懒构建：分析接口首次调用时通过 `ensure_graph_cache_built()` 构建邻接表与度数快照。
- 缓存失效：模块集合变化时调用 `invalidate_graph_cache()`，保证分析结果与当前集合一致。
- 算法：
  - 拓扑排序（Kahn）：入度为 0 队列，移除边生成序列。
  - 层级计算：按波次推进，记录每个节点层号与最大层宽。
  - 环检测：三色 DFS（白/灰/黑），灰色再入判定为环。
  - 关键路径：在 DAG 上用拓扑序进行最长路动态规划（常数或模块耗时权重）。
  - 路径计数：源点初始化为 1，沿拓扑序累加至汇点。
  - 最大并行宽度：取层级的最大节点数作为并行度上限近似值。
- 导出：
  - `export_dot()` 输出 DOT 的节点与依赖边（`dep -> node`）。
  - `export_csv_stats()` 输出模块耗时、内存估算与成功/失败标记的 CSV。

性能统计与内存估算
- 模块统计：记录开始/结束时间、耗时、是否成功、粗略内存占用。
- 工作流统计：记录总模块数、完成/失败数、总耗时、平均模块耗时、峰值内存，CPU 利用率当前为占位值。
- 估算方法：依据结果表大小与键长度推断，后续可替换为更精确的采样或由用户提供度量。

观察钩子
- 可设置 `on_start`、`on_complete`、`on_error` 钩子以观察模块生命周期。
- 建议钩子回调保持轻量与线程安全，避免阻塞与重计算。
- 示例 `15_workflow_features` 展示了记录事件时间线与错误快照的实践。

可扩展性与未来工作
- 取消与超时：为递归调度增加取消令牌与超时传播。
- 导出增强：提供 JSON/GraphML 导出与 Web 可视化支持。
- 调度策略：支持优先级、分层批次执行或多执行器协作。
- 统计完善：补充 CPU 利用率、统一失败路径统计与更精确内存度量。
- 路径与环细节：环检测回填具体环路，关键路径支持多源/多汇与权重组合。

局限与边界
- 结果采用 `std::any`，须调用方保证类型一致或使用 `TypedModule<T>` 提升类型安全。
- 大型图分析的缓存重建开销与内存占用需评估；
- 存在环时执行会失败，分析接口在环场景下返回空或负值以指示异常。

源码参考索引
- 接口：`include/concurrencpp/workflow/executor.h`、`include/concurrencpp/workflow/module.h`
- 调度实现：`source/workflow/executor.cpp`（`execute_async`、`execute_module_recursive`）
- 示例：`example/14_workflow_basic/source/main.cpp`、`example/15_workflow_features/source/main.cpp`、`example/16_workflow_analytics_demo/source/main.cpp`