# 集成计划

## P0（当前）
- 设计文档拆分：概览/模型/API/调度/协程/示例/集成。

## P1（实现与接入）
- 新增头文件：
  - `include/concurrencpp/workflow/node.h`
  - `include/concurrencpp/workflow/graph.h`
  - `include/concurrencpp/workflow/scheduler.h`
  - `include/concurrencpp/workflow/workflow.h`
- 顶层 `CMakeLists.txt` 安装导出上述头文件。
- 在 `include/concurrencpp/concurrencpp.h` 暴露 `#include <concurrencpp/workflow/workflow.h>`。
- 新增示例：`example/14_workflow_basic` 展示 DAG 串并混合。

## P2（增强）
- 整体与节点级超时；并发阈值；更细策略；单元测试覆盖。
- 协程友好：节点签名支持 `result<void>` 或 `generator<T>`。

## 构建与运行
- 项目根：`cmake -B build -S . && cmake --build build -j`。
- 运行示例：`build/example/14_workflow_basic/` 下执行目标程序。