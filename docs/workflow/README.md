# concurrencpp-X / workflow 设计文档索引

本目录拆分工作流模块的设计方案，避免单文件耦合，便于演进。

- 00_overview.md：目标与范围、总体架构、对标映射
- 01_model.md：DAG/节点/边/状态建模，串行/并行表达
- 02_api.md：头文件拆分与核心 API 设计（解耦方案）
- 03_scheduling.md：调度算法、错误/取消策略、超时机制
- 04_coroutines.md：结合 `result`/`generator`/`when_all` 的协程适配
- 05_examples.md：典型用例与示例图谱
- 06_integration_plan.md：落地与集成步骤（CMake/示例/测试）