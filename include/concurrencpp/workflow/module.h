#ifndef CONCURRENCPP_WORKFLOW_MODULE_H
#define CONCURRENCPP_WORKFLOW_MODULE_H

#include "concurrencpp/results/result.h"
#include "concurrencpp/executors/executor.h"
#include "concurrencpp/runtime/runtime.h"
#include <string>
#include <vector>
#include <memory>

namespace concurrencpp::workflow {

    class Module {
    private:
        std::string name_;
        std::vector<std::string> depend_;
        std::shared_ptr<concurrencpp::runtime> runtime_;
        std::shared_ptr<concurrencpp::executor> preferred_executor_;

    public:
        Module(const std::string& name, std::vector<std::string> depend = {})
            : name_(name), depend_(std::move(depend)) {}

        virtual ~Module() = default;

        const std::string& getName() const { return name_; }
        const std::vector<std::string>& getDepend() const { return depend_; }
        void addDepend(const std::string& dep) { depend_.push_back(dep); }
        void addDepends(std::vector<std::string> deps) {
            depend_.insert(depend_.end(), deps.begin(), deps.end());
        }

        void setRuntime(std::shared_ptr<concurrencpp::runtime> rt) { runtime_ = std::move(rt); }
        std::shared_ptr<concurrencpp::runtime> runtime() const { return runtime_; }

        // 设置/获取模块首选执行器（可为空表示不指定）
        void setPreferredExecutor(std::shared_ptr<concurrencpp::executor> ex) { preferred_executor_ = std::move(ex); }
        std::shared_ptr<concurrencpp::executor> preferred_executor() const { return preferred_executor_; }

        // 模块可根据自身类型选择合适的执行器，默认使用线程池
        virtual std::shared_ptr<concurrencpp::executor> select_executor(
            std::shared_ptr<concurrencpp::runtime> rt) const;

        // 可选：模块级超时控制（默认0表示不启用）
        virtual std::chrono::milliseconds timeout() const { return std::chrono::milliseconds{0}; }
        // 可选：模块是否支持/需要在取消信号时快速退出。由具体执行函数遵守。
        virtual bool cancellable() const { return false; }

        // 协作式取消钩子：当工作流请求取消时，上层会调用此方法。
        // 默认无操作，支持模块自行设置内部标志以便在 execute_async 中尽快返回。
        virtual void on_cancel();


        // 默认所有模块为 void，无需返回具体值
        virtual result<void> execute_async(std::shared_ptr<concurrencpp::executor> executor) = 0;

        void execute(std::shared_ptr<concurrencpp::executor> executor = nullptr);
    };

    using ModulePtr = std::shared_ptr<Module>;

} // namespace concurrencpp::workflow

#endif // CONCURRENCPP_WORKFLOW_MODULE_H