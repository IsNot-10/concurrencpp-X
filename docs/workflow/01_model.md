# 模型设计

## 节点（Node）
- 字段：`name`，`fn`（同步或协程包装），`executor`（可选），`state`（idle/running/success/failed/canceled），`dependents`（下游列表），`indegree`（入度计数）。
- 钩子：`on_start`，`on_complete`，`on_error`。
- 结果：节点执行得到 `concurrencpp::result<void>`；错误以 `std::exception_ptr` 聚合。

## 图（Graph）
- 存储：`std::vector<Node>` + 邻接表；`std::vector<size_t> indegree`；`std::queue<size_t> ready`。
- 约束：必须无环（DAG）；`add_edge(u, v)` 将 v 的入度 +1；构建后计算入度为 0 的初始就绪集。

## 串行与并行
- 串行：通过边约束自然表达序列（如 A→B→C）；也可提供 `Serial({A,B,C})` 组合器创建链式关系。
- 并行：同层入度为 0 的节点并行下发；也可提供 `Parallel({X,Y,Z})` 组合器显式并发，并使用 `when_all` 汇聚。

## 生命周期
- 状态迁移：`idle`→`running`→`success|failed|canceled`。
- 完成推进：对所有下游 `indegree--`，入度为 0 则加入就绪队列。
- 取消策略：错误发生时根据策略决定是否取消尚未启动的下游节点。