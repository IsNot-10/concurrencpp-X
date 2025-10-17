# 并发模式与协程适配

本节聚焦串行、并行与 DAG 调度在 `concurrencpp` 环境下的设计细节，以及如何利用 `result`/`when_all`/`when_any`/`resume_on`/`generator` 等协程工具实现高效组合。

## 串行（Serial）
- 语义：节点按顺序执行（A→B→C）。
- 构造：`Serial({A,B,C})` 组合器负责新增节点并创建链式边。
- 执行：通过拓扑约束实现串行，不需要额外的同步原语。
- 协程适配：若节点以协程包装，则在 `co_await` 完成后推进下游；可在 `resume_on(executor)` 切换执行器。

## 并行（Parallel）
- 语义：兄弟节点在其共同父节点完成后并发下发（A→{B,C,D}）。
- 构造：`Parallel({X,Y,Z})` 组合器创建多个并发节点；可选统一执行器或节点自定义执行器。
- 汇聚：下游节点 W 依赖 B/C/D 时，利用 `when_all({resB,resC,resD})` 协程聚合，确保三者完成后再触发 W。
- 协程适配：并发节点返回 `result<void>`；在汇聚节点处 `co_await when_all(...)`。

## DAG 调度
- 语义：任意有向无环图；入度为 0 的节点并发下发，完成后释放下游入度。
- 关键：入度计数、就绪队列、完成回调推进下游。
- 错误策略：
  - `continue_on_error`：记录异常并继续推进不受影响的分支。
  - `cancel_on_error`：取消尚未启动且可达的下游分支，避免错误扩散与资源浪费。
- 超时：整体超时使用 `timer_queue` oneshot 定时器；节点级超时可在节点内部通过 `delay_object` 或包装 `result.timeout(...)` 实现。

## 协程工具使用要点
- `result<T>`：
  - 节点提交：`executor->submit(fn)` 返回 `result<void>`；
  - 等待：`result.get()` 阻塞或 `co_await result` 非阻塞等待。
- `when_all`：
  - 并行分支汇聚，适合多个兄弟节点的完成协调；
  - 在协程中 `co_await when_all(std::move(results))`。
- `when_any`：
  - 选择器/竞态场景；某一分支先完成即可推进后续逻辑。
- `resume_on(executor)`：
  - 切换协程恢复到指定执行器（如线程池），确保后续 `await` 在正确线程上下文。
- `generator<T>`：
  - 流式/背压型节点；可作为扩展在流水线中逐步产出数据并让下游消费。

## 示例片段

串并混合 + 汇聚：
```
// 父节点 A
size_t A = wf.add_task("A", [] { /* do A */ });

// 并发分支 B/C/D
auto ids = concurrencpp::workflow::Parallel(
    wf,
    {
      {"B", [] { /* do B */ }},
      {"C", [] { /* do C */ }},
      {"D", [] { /* do D */ }}
    }, pool);

// 依赖边：A -> B,C,D
for (auto id : ids) wf.add_edge(A, id);

// 汇聚节点 W（依赖 B/C/D）
size_t W = wf.add_task("W", [] {
  // 可在调度器内部以 when_all 聚合触发 W；
  // 若以协程包装：co_await when_all(resB,resC,resD);
});
for (auto id : ids) wf.add_edge(id, W);
```

协程包装节点（示意）：
```
auto make_async = [&](std::function<void()> fn,
                      std::shared_ptr<concurrencpp::executor> ex) -> concurrencpp::result<void> {
  co_await concurrencpp::resume_on(ex);
  fn();
  co_return;
};
```

## 取消与一致性
- 取消传播：默认“取消全下游”；后续可提供“仅直接下游”“可达下游”等策略配置。
- 一致性：在 `cancel_on_error` 下游不应被下发，调度器需标记并跳过；在 `continue_on_error` 模式，非依赖错误分支持续推进。

## 性能与扩展
- 并发度由执行器决定（线程池可调）；可以提供逻辑并发阈值限制同时在运行的节点数。
- 优先级：可按拓扑层或自定义权重排序就绪队列（后续拓展）。
- 观测：钩子 `on_start/on_complete/on_error` 收集指标；后续可导出 DOT 或统计数据。