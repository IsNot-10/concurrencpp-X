#include "concurrencpp/workflow/module.h"
#include "concurrencpp/runtime/runtime.h"
#include "concurrencpp/executors/thread_pool_executor.h"

using namespace concurrencpp::workflow;

void Module::execute(std::shared_ptr<concurrencpp::executor> executor) {
    auto res = execute_async(std::move(executor));
    res.get();
}

std::shared_ptr<concurrencpp::executor> Module::select_executor(std::shared_ptr<concurrencpp::runtime> rt) const {
    // 若已设置首选执行器则优先使用
    if (preferred_executor_) {
        return preferred_executor_;
    }
    // 否则回退到 runtime 的线程池；rt 为空则返回空指针，交由上层回退到默认执行器
    if (!rt) {
        return nullptr;
    }
    auto pool = rt->thread_pool_executor();
    return std::static_pointer_cast<concurrencpp::executor>(pool);
}

void Module::on_cancel() {
    // 默认空实现。具体模块可重载并在内部设置标志用于协作式退出。
}