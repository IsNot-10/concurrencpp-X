目标
- 在 `concurrencpp-X` 内新增一个“任务流调度”模块（workflow），不引入第三方库或文件。
- 设计以 DAG（有向无环图）为核心的任务编排与调度，利用 `concurrencpp` 的 executors、results、timers 实现并发与异步。
- 参考 `/home/abab/project/workflow` 的功能思想与常用模式，但不复制代码；以最小可用集为起点，逐步迭代增强。

范围与非目标
- 范围：静态 DAG 建模、并发调度、错误传播/取消策略、可选超时控制、基本观测/事件钩子。
- 非目标（首版不做）：动态增删节点、流式 IO 客户端/服务端、复杂网络协议、跨进程通信等。

总体架构
- 命名空间：`concurrencpp::workflow`
- 组成：
  - 图模型：节点（任务）与边（依赖），入度计数、就绪队列。
  - 调度器：根据依赖拓扑推进，就绪任务通过 `executor` 并发执行，完成后释放下游任务入度。
  - 执行器：默认使用 `runtime.make_thread_pool_executor()`，节点可覆盖自定义执行器。
  - 结果与错误：节点执行返回 `concurrencpp::result<void>`；错误通过 `std::exception_ptr` 聚合并按策略处理。
  - 计时器（可选）：基于 `runtime.timer_queue()` 提供工作流整体或节点超时。

最小可用 API 设计（MVP）
- 头文件位置：`include/concurrencpp/workflow/workflow.h`（首版建议 header-only，后续视需要拆 cpp）。
- 类型与接口（示意）：
  - `enum class WorkflowErrorPolicy { continue_on_error, cancel_on_error };`
  - `struct WorkflowTask { std::string name; std::function<void()> fn; std::shared_ptr<concurrencpp::executor> exec; /* state, deps, etc. */ };`
  - `class Workflow {
      public:
        explicit Workflow(std::shared_ptr<concurrencpp::executor> default_exec = nullptr);
        size_t add_task(std::string name, std::function<void()> fn, std::shared_ptr<concurrencpp::executor> exec = nullptr);
        void add_edge(size_t parent, size_t child);
        void set_error_policy(WorkflowErrorPolicy p);
        void set_max_concurrency(size_t n); // 逻辑并行阈值（非必需）
        void set_timeout(std::chrono::milliseconds); // 工作流整体超时（可选）
        // 钩子：
        void on_start(std::function<void(const WorkflowTask&)> cb);
        void on_complete(std::function<void(const WorkflowTask&)> cb);
        void on_error(std::function<void(const WorkflowTask&, const std::exception&)> cb);
        concurrencpp::result<void> run(concurrencpp::runtime& rt);
        void reset();
    };
`
- 约定：
  - 节点 `fn` 为同步函数，调度器用 `executor->submit(fn)` 获取 `result<void>` 并串联推进；后续可扩展为协程签名。
  - `run` 返回当所有任务完成或被取消时完成的 `result<void>`。
  - 错误策略：
    - `continue_on_error`：记录错误但不取消下游，适用于“尽可能完成”。
    - `cancel_on_error`：遇到任一失败即取消尚未开始的下游任务。

执行流程（示意）
1. 构建 DAG：`add_task` + `add_edge`，计算每个节点入度。
2. 初始化：将入度为 0 的节点放入就绪队列。
3. 调度：从就绪队列取出节点，选择节点执行器（优先节点自带，否则默认），`submit(fn)`；标记运行中。
4. 完成回调：节点完成后，触发 `on_complete`，对每个下游节点入度减 1，入度为 0 则加入就绪队列。
5. 错误回调：捕获异常触发 `on_error`；依据策略决定是否标记下游为取消并不再下发。
6. 终止：所有节点到达终态（完成/取消/失败）或超时触发；汇总错误并完成 `run` 的 `result`。

对标 `/home/abab/project/workflow` 的功能映射
- Graph/DAG：首版支持静态 DAG；后续可考虑 `WFGraphTask` 的动态扩展。
- Scheduler/Executor：以 `concurrencpp` 的 `thread_pool_executor` 等作为执行后端；不实现自定义内核线程池。
- 错误与取消：提供继续/取消两种策略；后续引入“条件分支”“选择器”理念可对齐 `workflow` 的 selector/operator。
- 计时器/超时：用 `timer_queue` 支持工作流整体或节点级超时（节点级超时可在 `fn` 内通过 `delay_object`/`result.timeout` 组合实现）。
- 观测与事件：提供 on_start/on_complete/on_error 钩子；后续可扩展统计计数与 DOT 导出。

集成计划
- P0（本次）：
  - 在 `include/concurrencpp/workflow/workflow.h` 提供 header-only MVP，实现 DAG 调度、错误策略、基本钩子。
  - 根目录新增此设计文档，先冻结方案。
- P1：
  - 在顶层 `CMakeLists.txt` 安装/导出 `workflow.h`；在 `include/concurrencpp/concurrencpp.h` 暴露 `#include <concurrencpp/workflow/workflow.h>`。
  - 新增示例 `example/14_workflow_basic`：构建一个 A→B、A→C→D 的简单 DAG 并运行。
- P2：
  - 引入整体/节点超时；在 `run` 内集成 cancel token 或简单标志；补充单元测试。
  - 支持节点并行阈值与简单优先级（可选）。
- P3：
  - 条件分支/选择器、动态子图、DOT 导出、统计与诊断（可选）。

并发与同步策略
- 图状态保护：`std::mutex` + 原子计数（入度/完成数）；避免忙等。
- 结果收集：集中维护 `std::vector<std::exception_ptr>`；`cancel_on_error` 下游通过状态位阻止下发。
- 资源释放：`reset()` 清理状态以便重复运行同一图。

示例（伪代码）
```
#include <concurrencpp/runtime/runtime.h>
#include <concurrencpp/workflow/workflow.h>

int main() {
    concurrencpp::runtime rt;
    auto pool = rt.make_thread_pool_executor();
    concurrencpp::workflow::Workflow wf(pool);

    auto a = wf.add_task("A", [] { /* do A */ });
    auto b = wf.add_task("B", [] { /* do B */ });
    auto c = wf.add_task("C", [] { /* do C */ });
    auto d = wf.add_task("D", [] { /* do D */ });

    wf.add_edge(a, b);
    wf.add_edge(a, c);
    wf.add_edge(c, d);

    wf.set_error_policy(concurrencpp::workflow::WorkflowErrorPolicy::cancel_on_error);

    auto r = wf.run(rt);
    r.get(); // 阻塞直到工作流完成
}
```

验收与测试
- 单元测试：
  - 正常拓扑执行与结果完成。
  - 节点失败在两种策略下的传播行为。
  - 超时触发后的取消效果（加入后）。
- 示例运行：`example/14_workflow_basic` 构建与执行，确保与 `sandbox` 同样的构建路径与命令。

风险与权衡
- 选择同步 `fn` + `executor->submit` 的简单模型，便于落地；如需更强的协程表达，再引入 `generator`/`co_await` 版本。
- 取消传播在复杂图中需要明确语义（是否仅取消直接下游、还是全下游），首版采用“取消全下游”的直观策略。
- 超时细粒度（节点级）可能需要额外控制通道；首版优先工作流整体超时。

后续工作
- 评审方案后实现 `workflow.h` MVP，并补充示例与 CMake 接入。
- 迭代 API 与行为，逐步对齐 `/home/abab/project/workflow` 的高级能力。