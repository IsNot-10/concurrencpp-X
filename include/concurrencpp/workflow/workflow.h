#ifndef CONCURRENCPP_WORKFLOW_H
#define CONCURRENCPP_WORKFLOW_H

/**
 * @file workflow.h
 * @brief ConcurrenCpp工作流模块主头文件
 * 
 * 基于DAG（有向无环图）的任务调度框架，提供模块化的任务执行能力。
 * 支持复杂的依赖关系管理和自动的执行顺序调度。
 */

#include "module.h"
#include "executor.h"
#include <memory>
#include <utility>

// 说明：本头文件作为聚合入口，仅提供模块与执行器的统一包含。

#endif // CONCURRENCPP_WORKFLOW_H