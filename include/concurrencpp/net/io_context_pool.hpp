#ifndef ASYNC_SIMPLE_DEMO_IO_CONTEXT_POOL_H
#define ASYNC_SIMPLE_DEMO_IO_CONTEXT_POOL_H
#include <memory>
#include <thread>
#include <vector>
#include "concurrencpp/net/asio/asio.hpp"

class io_context_pool {
public:
    explicit io_context_pool(std::size_t pool_size) : next_io_context_(0) {
        if (pool_size == 0)
            pool_size = 1;  // set default value as 1

        for (std::size_t i = 0; i < pool_size; ++i) {
            io_context_ptr io_context(new asio::io_context);
            work_ptr work(new asio::io_context::work(*io_context));
            io_contexts_.push_back(io_context);
            work_.push_back(work);
        }
    }

    void run() {
        std::vector<std::shared_ptr<std::thread>> threads;
        for (std::size_t i = 0; i < io_contexts_.size(); ++i) {
            threads.emplace_back(std::make_shared<std::thread>(
                [](io_context_ptr svr) { svr->run(); }, io_contexts_[i]));
        }

        for (std::size_t i = 0; i < threads.size(); ++i)
            threads[i]->join();
    }

    void stop() {
        work_.clear();

        for (std::size_t i = 0; i < io_contexts_.size(); ++i)
            io_contexts_[i]->stop();
    }

    size_t current_io_context() { 
        if (next_io_context_ == 0) {
            return io_contexts_.size() - 1;
        }
        return next_io_context_ - 1; 
    }

    asio::io_context &get_io_context() {
        asio::io_context &io_context = *io_contexts_[next_io_context_];
        ++next_io_context_;
        if (next_io_context_ == io_contexts_.size())
            next_io_context_ = 0;
        return io_context;
    }

private:
    using io_context_ptr = std::shared_ptr<asio::io_context>;
    using work_ptr = std::shared_ptr<asio::io_context::work>;

    std::vector<io_context_ptr> io_contexts_;
    std::vector<work_ptr> work_;
    std::size_t next_io_context_;
};

#endif
