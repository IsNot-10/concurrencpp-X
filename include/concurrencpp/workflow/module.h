#ifndef CONCURRENCPP_WORKFLOW_MODULE_H
#define CONCURRENCPP_WORKFLOW_MODULE_H

#include "concurrencpp/results/result.h"
#include "concurrencpp/executors/executor.h"
#include "concurrencpp/runtime/runtime.h"
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <stdexcept>
#include "param.h"

namespace concurrencpp::workflow {

    class Module {
    private:
        std::string name_;
        std::vector<std::string> depend_;
        std::shared_ptr<concurrencpp::runtime> runtime_;
        std::shared_ptr<concurrencpp::executor> preferred_executor_;
        std::shared_ptr<ParamStore> params_;
        // 协作式取消/暂停状态与同步原语
        std::atomic_bool canceled_{false};
        std::atomic_bool suspended_{false};
        mutable std::mutex suspend_mtx_;
        mutable std::condition_variable suspend_cv_;

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
            std::shared_ptr<concurrencpp::runtime> rt) const {
            if (preferred_executor_) return preferred_executor_;
            if (rt) return std::static_pointer_cast<concurrencpp::executor>(rt->thread_pool_executor());
            return nullptr;
        }

        // 可选：模块级超时控制（默认0表示不启用）
        virtual std::chrono::milliseconds timeout() const { return std::chrono::milliseconds{0}; }
        // 可选：模块是否支持/需要在取消信号时快速退出。由具体执行函数遵守。
        virtual bool cancellable() const { return false; }

        // 协作式取消/暂停钩子
        virtual void on_cancel() {
            canceled_.store(true, std::memory_order_relaxed);
            suspend_cv_.notify_all();
        }
        virtual void on_suspend() {
            suspended_.store(true, std::memory_order_relaxed);
        }
        virtual void on_resume() {
            suspended_.store(false, std::memory_order_relaxed);
            suspend_cv_.notify_all();
        }

        // 默认所有模块为 void，无需返回具体值
        virtual result<void> execute_async(std::shared_ptr<concurrencpp::executor> executor) = 0;

        // 同步执行便捷封装（阻塞等待异步结果完成）
        void execute(std::shared_ptr<concurrencpp::executor> executor = nullptr) {
            auto ex = executor ? executor : select_executor(runtime_);
            if (!ex) {
                throw std::runtime_error("workflow::Module::execute: no executor available");
            }
            auto r = execute_async(ex);
            r.get();
        }

        // 共享参数存储注入与访问
        void setParamStore(std::shared_ptr<ParamStore> ps) { params_ = std::move(ps); }
        std::shared_ptr<ParamStore> param_store() const { return params_; }

        // 便捷读写：模块间共享参数
        template <class T>
        void set_param(const std::string& key, T&& value) {
            if (!params_) throw std::runtime_error("workflow::Module: param store is null");
            params_->set<T>(key, std::forward<T>(value));
        }
        template <class T, class... Args>
        void emplace_param(const std::string& key, Args&&... args) {
            if (!params_) throw std::runtime_error("workflow::Module: param store is null");
            params_->emplace<T>(key, std::forward<Args>(args)...);
        }
        template <class T>
        void set_shared_param(const std::string& key, std::shared_ptr<T> ptr) {
            if (!params_) throw std::runtime_error("workflow::Module: param store is null");
            params_->set_shared<T>(key, std::move(ptr));
        }
        template <class T>
        std::shared_ptr<T> get_param(const std::string& key) const {
            if (!params_) throw std::runtime_error("workflow::Module: param store is null");
            return params_->get<T>(key);
        }
        bool param_exists(const std::string& key) const {
            if (!params_) return false;
            return params_->exists(key);
        }
        template <class T, class Fn>
        void with_read_param(const std::string& key, Fn&& fn) const {
            if (!params_) throw std::runtime_error("workflow::Module: param store is null");
            params_->with_read<T>(key, std::forward<Fn>(fn));
        }
        template <class T, class Fn>
        void with_write_param(const std::string& key, Fn&& fn) {
            if (!params_) throw std::runtime_error("workflow::Module: param store is null");
            params_->with_write<T>(key, std::forward<Fn>(fn));
        }

    protected:
        // 协作式暂停等待：处于 SUSPEND 时阻塞，直到被恢复或取消
        void check_suspend() {
            std::unique_lock<std::mutex> lk(suspend_mtx_);
            suspend_cv_.wait(lk, [&]{
                return !suspended_.load(std::memory_order_relaxed) || canceled_.load(std::memory_order_relaxed);
            });
        }
        bool cancel_requested() const { return canceled_.load(std::memory_order_relaxed); }
        bool is_suspended() const { return suspended_.load(std::memory_order_relaxed); }
    };

    using ModulePtr = std::shared_ptr<Module>;

} // namespace concurrencpp::workflow

#endif // CONCURRENCPP_WORKFLOW_MODULE_H