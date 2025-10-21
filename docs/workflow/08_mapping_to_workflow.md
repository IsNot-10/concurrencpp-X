# 与 `/home/abab/project/workflow` 的功能映射

本节将 `concurrencpp-X/workflow` 设计与参考项目 `workflow` 的核心能力进行对位，确保方向一致但不引入其第三方代码。

## 核心对象对位
- 任务（Node）≈ `WFTask`/`WFOperator`
  - 均表示可执行单元。我们的 `Node` 以 `std::function<void()>` 为最小签名，后续可扩展为协程包装与返回 `result<void>` 或 `generator<T>`。
- 图（Graph）≈ `WFGraphTask`
  - DAG 存储与推进。我们以入度计数 + 邻接表 + 就绪队列实现拓扑推进。
- 调度器（Scheduler）≈ `Workflow`/调度内核
  - 负责选择执行器、提交任务、处理完成与错误传播、执行取消与超时等核心流程。
- 执行器（concurrencpp::executor）≈ `Executor`
  - 我们直接使用 `concurrencpp` 的线程池/线程执行器，不自研线程池。
- 结果与组合（concurrencpp::result / when_all / when_any）≈ `WFFuture`/组合
  - 我们用 `result<T>` 表示节点完成，可用 `when_all`/`when_any` 聚合并行分支，语义上对应参考项目的组合与选择器。
- 定时器与超时（timer_queue）≈ `WFTimer`/超时
  - 我们以 `timer_queue` 实现整体超时；节点级超时可在后续通过包装实现。

## 串行 / 并行 / 汇聚
- 串行（Serial）：与参考项目的顺序链一致，使用边约束（A→B→C）；提供 `Serial` 组合器简化构造。
- 并行（Parallel）：在父节点完成后并发下发多个兄弟节点；使用 `Parallel` 组合器；在后续汇聚节点使用 `when_all` 保证全部完成。
- 选择器（Selector）：可用 `when_any` 实现竞态推进（某一分支先完成即可继续）。首版不提供复杂选择器语法糖，仅在调度器内部支持。

## 错误与取消策略
- 继续（continue_on_error）：记录异常但不阻断不受影响的分支。
- 取消（cancel_on_error）：取消尚未启动的可达下游分支，避免资源浪费与错误扩散。
- 异常聚合：集中维护 `std::vector<std::exception_ptr>`，在 `run` 的最终结果中上抛或归纳。

## 超时语义
- 整体超时：oneshot 定时器触发后终止尚未启动的节点；将整体结果置为超时错误。
- 节点级超时（后续）：在节点包装上支持 `result.timeout(...)` 或 `delay_object`，并在调度器中统一处理。

## 扩展方向（对齐参考项目）
- 条件分支/选择器：引入基于 `when_any` 与用户谓词的路由；
- 动态子图：运行期生成子图并纳入调度；
- 统计与观测：钩子收集指标与 DOT 导出；
- 优先级与配额：就绪队列按权重或层级排序，并可设定逻辑并发阈值。

## 不引入第三方的约束
- 仅使用 `concurrencpp` 自带的 executors/results/timers 与 C++ 标准库；
- 头文件解耦：`node.h`、`graph.h`、`scheduler.h`、`workflow.h` + 可选组合器 `serial.h`、`parallel.h`；
- 示例与测试与现有项目构建方式一致（CMake 示例与 test）。