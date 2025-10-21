#ifndef CONCURRENCPP_WORKFLOW_PARAM_H
#define CONCURRENCPP_WORKFLOW_PARAM_H

#include <unordered_map>
#include <string>
#include <memory>
#include <typeinfo>
#include <typeindex>
#include <shared_mutex>
#include <stdexcept>
#include <utility>
#include <type_traits>
#include <vector>

namespace concurrencpp::workflow {

    // 线程安全的参数存储，用于模块之间共享数据（分片降低锁争用）
    class ParamStore {
       private:
        struct Entry {
            std::shared_ptr<void> ptr;
            std::type_index type{typeid(void)};
        };
        struct Shard {
            std::unordered_map<std::string, Entry> map;
            mutable std::shared_mutex mutex;
        };
        // 使用指针容器，避免对包含 shared_mutex 的 Shard 进行移动/复制
        std::vector<std::unique_ptr<Shard>> m_shards;
        size_t m_mask{0};

        size_t pick(const std::string& key) const noexcept {
            return std::hash<std::string>{}(key) & m_mask;
        }

       public:
        explicit ParamStore(size_t shard_count = 16) {
            size_t n = 1;
            while (n < shard_count) n <<= 1;
            m_shards.resize(n);
            for (size_t i = 0; i < n; ++i) {
                m_shards[i] = std::make_unique<Shard>();
            }
            m_mask = n - 1;
        }
        ~ParamStore() = default;

        // 以值写入（复制/移动），自动记录类型
        template <class T>
        void set(const std::string& key, T&& value) {
            using U = std::decay_t<T>;
            auto& sh = *m_shards[pick(key)];
            std::unique_lock lk(sh.mutex);
            Entry e{std::make_shared<U>(std::forward<T>(value)), std::type_index(typeid(U))};
            sh.map.insert_or_assign(key, std::move(e));
        }

        // 原位构造写入
        template <class T, class... Args>
        void emplace(const std::string& key, Args&&... args) {
            auto& sh = *m_shards[pick(key)];
            std::unique_lock lk(sh.mutex);
            Entry e{std::make_shared<T>(std::forward<Args>(args)...), std::type_index(typeid(T))};
            sh.map.insert_or_assign(key, std::move(e));
        }

        // 以共享指针写入（复用外部所有权）
        template <class T>
        void set_shared(const std::string& key, std::shared_ptr<T> ptr) {
            if (!ptr) {
                throw std::runtime_error("ParamStore::set_shared - null shared_ptr for key: " + key);
            }
            auto& sh = *m_shards[pick(key)];
            std::unique_lock lk(sh.mutex);
            Entry e{std::move(ptr), std::type_index(typeid(T))};
            sh.map.insert_or_assign(key, std::move(e));
        }

        template <class T>
        std::shared_ptr<T> get(const std::string& key) const {
            auto& sh = *m_shards[pick(key)];
            std::shared_lock lk(sh.mutex);
            auto it = sh.map.find(key);
            if (it == sh.map.end()) {
                throw std::runtime_error("ParamStore::get - param not found: " + key);
            }
            if (it->second.type != std::type_index(typeid(T))) {
                throw std::runtime_error("ParamStore::get - type mismatch for key: " + key);
            }
            return std::static_pointer_cast<T>(it->second.ptr);
        }

        bool exists(const std::string& key) const {
            auto& sh = *m_shards[pick(key)];
            std::shared_lock lk(sh.mutex);
            return sh.map.find(key) != sh.map.end();
        }

        void erase(const std::string& key) {
            auto& sh = *m_shards[pick(key)];
            std::unique_lock lk(sh.mutex);
            sh.map.erase(key);
        }

        void clear() {
            for (auto& shp : m_shards) {
                std::unique_lock lk(shp->mutex);
                shp->map.clear();
            }
        }

        // 在读锁下执行只读访问函数
        template <class T, class Fn>
        void with_read(const std::string& key, Fn&& fn) const {
            auto& sh = *m_shards[pick(key)];
            std::shared_lock lk(sh.mutex);
            auto it = sh.map.find(key);
            if (it == sh.map.end()) {
                throw std::runtime_error("ParamStore::with_read - param not found: " + key);
            }
            if (it->second.type != std::type_index(typeid(T))) {
                throw std::runtime_error("ParamStore::with_read - type mismatch for key: " + key);
            }
            fn(*std::static_pointer_cast<T>(it->second.ptr));
        }

        // 在写锁下执行可修改访问函数
        template <class T, class Fn>
        void with_write(const std::string& key, Fn&& fn) {
            auto& sh = *m_shards[pick(key)];
            std::unique_lock lk(sh.mutex);
            auto it = sh.map.find(key);
            if (it == sh.map.end()) {
                throw std::runtime_error("ParamStore::with_write - param not found: " + key);
            }
            if (it->second.type != std::type_index(typeid(T))) {
                throw std::runtime_error("ParamStore::with_write - type mismatch for key: " + key);
            }
            fn(*std::static_pointer_cast<T>(it->second.ptr));
        }
    };

} // namespace concurrencpp::workflow

#endif // CONCURRENCPP_WORKFLOW_PARAM_H