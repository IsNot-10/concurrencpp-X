# 面试亮点与使用示例（Workflow）

这套示例基于 `concurrencpp` 协程库，实现了一个轻量的异步 DAG 工作流执行器（Executor + TypedModule），支持并行执行、依赖解析、性能统计与内存安全优化，适合在面试中作为工程化与并发能力的展示。

## 可吹点（亮点）

- 轻量并发架构：使用 C++20 `coroutine`、`lazy_result`、`resume_on` 实现零样板的异步编排。
- 类型安全的模块系统：`TypedModule<T>` + `getDependencyResult<T>`，在保留跨模块 `std::any` 灵活性的同时保证依赖访问的类型安全。
- DAG 并行执行：自动解析依赖，独立模块并行运行，整体执行由 `Executor` 协调。
- 内存安全优化：执行完成后主动清理模块依赖指针；`Executor::clear()` 全面释放状态与统计，避免悬空引用与潜在泄漏。
- 诊断友好：内置错误收集（`ModuleError` 列表）、执行状态与耗时统计（工作流与单模块维度）。
- ASAN 一键开关：示例层支持 `AddressSanitizer`（`-DENABLE_ASAN=ON`），便于现场演示内存问题拦截能力。
- 运行标识与剖析开关：`--run-id` 标记一次执行；`--profile` 输出详细的工作流与单模块性能统计。
- UBSAN 开关：示例层支持 `UndefinedBehaviorSanitizer`（`-DENABLE_UBSAN=ON`），展示未定义行为的发现能力。
- 结构化指标输出：`--json` 直接输出结构化性能与结果指标，可用于接入仪表盘/可视化。
- 多次重复与聚合：`--repeat=N` 支持重复执行并生成聚合统计（平均/最佳/最差），体现工程化评估能力。
- 静默/可追踪模式：`--quiet` 输出最小化（适合 CI）；`--trace` 明确展示执行路径与关键诊断开关。

## 如何构建与运行

1) 构建库（如需）：

```
cmake -S concurrencpp-X -B concurrencpp-X/build
cmake --build concurrencpp-X/build -j
```

2) 构建示例（可选启用 ASAN）：

```
cmake -S concurrencpp-X/example/14_workflow_basic -B concurrencpp-X/build-examples/14_workflow_basic -DENABLE_ASAN=ON
cmake --build concurrencpp-X/build-examples/14_workflow_basic -j
```

3) 运行并输出剖析信息与执行标识：

```
concurrencpp-X/build-examples/14_workflow_basic/workflow_basic_example --profile --run-id=job-001
```

4) 结构化输出与静默模式（适合面试演示/CI 收集）：

```
# 输出 JSON 指标并静默其他信息（仅打印 JSON）：
concurrencpp-X/build-examples/14_workflow_basic/workflow_basic_example --json --quiet --run-id=ci-123 > metrics.json

# 重复执行并给出聚合汇总：
concurrencpp-X/build-examples/14_workflow_basic/workflow_basic_example --profile --repeat=3 --run-id=bench-001
```

## 现场讲解要点（可加强表达）

- 设计分层：`Executor` 负责调度与依赖解析，`Module` 专注业务逻辑，模块和调度解耦，便于扩展与测试。
- 异步零样板：模块只需实现 `execute_typed`，调度层统一使用 `lazy_result` 进行 await；可接入不同执行器（线程池、Worker 等）。
- 安全性保证：执行结束与异常路径都置空依赖指针，`clear()` 归零状态，峰值内存统计用于容量评估与泄漏初筛；支持 ASAN/UBSAN 强化诊断。
- 可观测性：`--profile` 输出总览与逐模块指标（耗时/内存/成功失败），`--run-id` 用于复现场景与结果比对。
- 兼容扩展：可小步添加 `Executor::reset()`、进度监控（已有相关示例）、更精确的内存计量（针对 `std::any` 实际载荷做 tracking）。
- 指标工程化：`--json` 输出直接可接入日志系统或可视化平台（如 Grafana/Elastic），`--quiet` 适配 CI。
- 稳健演示：提供 ASAN/UBSAN 开关与聚合性能测量，展示“诊断→验证→复盘”的工程流程意识。

## 相关改动位置

- 示例构建开关：`concurrencpp-X/example/14_workflow_basic/CMakeLists.txt`（`ENABLE_ASAN`、`ENABLE_UBSAN`）。
- 示例入口：`concurrencpp-X/example/14_workflow_basic/source/main.cpp`（`--profile`、`--run-id`、`--json`、`--repeat`、`--quiet`、`--trace`）。
- 执行器内存优化：`concurrencpp-X/source/workflow/executor.cpp`（清理依赖指针与完善 `clear()`）。

以上改动均为“浅迭代”，不大刀阔斧，且已编译运行验证通过，适合面试场景演示与讲解。

## 面试可进一步加分的安全改动（小步迭代，低风险）

- 进度/可视化：增加简单进度条或百分比输出（不改库，仅示例层）。
- 参数化并发度：示例支持 `--max-workers`，展示对执行器并发度的可控实验能力。
- 可重复性：增加 `--seed` 以控制可重复实验场景（随机任务模拟）。
- 失败策略：增加 `--fail-fast`（首次失败即终止）与 `--retry=N`（示例层重试）用于工程化策略演示。
- 指标导出：支持 `--export=csv` 或按 JSON Schema 输出，便于和外部工具联动（保持示例改动）。